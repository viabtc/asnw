/*
 * Description: a server that spreads its connections over several threads
 *     History: yang@haipo.me, 2026/11/25, create
 */

# include <stdlib.h>
# include <string.h>
# include <unistd.h>
# include <signal.h>

# include "nw_hub.h"

/* the worker index lives in the top bits of the connection id */
# define ID_SHIFT 48

enum {
    EVENT_ADD_FD,
    EVENT_SEND,
    EVENT_CLOSE,
    EVENT_SHUTDOWN,
    EVENT_BROADCAST,
    EVENT_CALL,
    EVENT_EXIT,
};

/* data of a broadcast, one copy shared by all the workers */
struct shared_data {
    int refs;
    size_t size;
    char data[];
};

/* a request to a worker, made by another thread */
struct event {
    struct event *next;
    int type;
    /* EVENT_ADD_FD */
    int fd;
    uint32_t bind_index;
    /* EVENT_SEND, EVENT_CLOSE, EVENT_SHUTDOWN */
    uint64_t id;
    /* EVENT_BROADCAST, or EVENT_CALL for all workers */
    struct shared_data *shared;
    /* EVENT_CALL */
    nw_hub_callback callback;
    /* EVENT_SEND, EVENT_CALL: the data. EVENT_ADD_FD: the peer address */
    size_t size;
    char data[];
};

enum {
    WORKER_STARTING,
    WORKER_READY,
    WORKER_FAILED,
};

struct nw_hub_worker {
    nw_hub *hub;
    uint32_t index;
    pthread_t thread;
    bool thread_created;
    /* set once by the thread itself, under hub->lock */
    int state;
    /* the rest belongs to the worker thread */
    nw_svr *svr;
    struct ev_loop *loop;
    bool exit;
    /* the queue: any thread appends under lock and wakes the worker up.
     * open is false before the worker is ready and once it is ending, when
     * there is nobody left to wake up: requests are refused then */
    ev_async wakeup;
    pthread_mutex_t lock;
    bool open;
    struct event *head;
    struct event *tail;
    size_t queued_size;
};

/* the worker the calling thread is, NULL for any other thread */
static __thread struct nw_hub_worker *current;
/* the hubs the calling thread created and accepts for */
static __thread nw_hub *hubs_here;

static void shared_unref(struct shared_data *shared)
{
    if (__sync_sub_and_fetch(&shared->refs, 1) == 0)
        free(shared);
}

static struct event *event_new(int type, size_t size)
{
    struct event *event = malloc(sizeof(struct event) + size);
    if (event == NULL)
        return NULL;
    memset(event, 0, sizeof(struct event));
    event->type = type;
    event->fd = -1;
    event->size = size;
    return event;
}

/* what a request carries along is given back when it is not carried out */
static void event_drop(struct event *event)
{
    if (event->type == EVENT_ADD_FD && event->fd >= 0)
        close(event->fd);
    if (event->shared)
        shared_unref(event->shared);
    free(event);
}

/* the size a request counts for against queue_limit */
static size_t event_cost(struct event *event)
{
    if (event->shared)
        return event->shared->size;
    if (event->type == EVENT_SEND || event->type == EVENT_CALL)
        return event->size;
    return 0;
}

/* hand a request to a worker. return -1 if its queue is full or it is
 * ending, the event is then still the caller's */
static int worker_post(struct nw_hub_worker *worker, struct event *event)
{
    size_t cost = event_cost(event);
    size_t limit = worker->hub->queue_limit;

    pthread_mutex_lock(&worker->lock);
    if (!worker->open || (cost && limit && worker->queued_size + cost > limit)) {
        pthread_mutex_unlock(&worker->lock);
        return -1;
    }
    worker->queued_size += cost;
    if (worker->tail) {
        worker->tail->next = event;
    } else {
        worker->head = event;
    }
    worker->tail = event;
    /* inside the lock: the loop is sure to exist while open is true */
    ev_async_send(worker->loop, &worker->wakeup);
    pthread_mutex_unlock(&worker->lock);
    return 0;
}

/* take everything that is queued, in one go */
static struct event *worker_take(struct nw_hub_worker *worker)
{
    pthread_mutex_lock(&worker->lock);
    struct event *head = worker->head;
    worker->head = NULL;
    worker->tail = NULL;
    worker->queued_size = 0;
    pthread_mutex_unlock(&worker->lock);
    return head;
}

static void broadcast_here(nw_svr *svr, const void *data, size_t size)
{
    nw_svr_iterator iter;
    nw_ses *ses;
    nw_svr_iter_init(&iter, svr);
    while ((ses = nw_svr_next(&iter)) != NULL) {
        nw_ses_send(ses, data, size);
    }
}

static void worker_handle(struct nw_hub_worker *worker, struct event *event)
{
    nw_svr *svr = worker->svr;
    nw_ses *ses;

    switch (event->type) {
    case EVENT_ADD_FD:
        if (!worker->exit && nw_svr_add_accepted_fd(svr, event->bind_index, event->fd, (nw_addr *)event->data) == 0) {
            /* the connection has it now */
            event->fd = -1;
        }
        break;
    case EVENT_SEND:
        nw_svr_send(svr, event->id, event->data, event->size);
        break;
    case EVENT_CLOSE:
        ses = nw_svr_get_clt(svr, event->id);
        if (ses)
            nw_svr_close_clt(svr, ses);
        break;
    case EVENT_SHUTDOWN:
        ses = nw_svr_get_clt(svr, event->id);
        if (ses)
            nw_svr_shutdown_clt(svr, ses);
        break;
    case EVENT_BROADCAST:
        broadcast_here(svr, event->shared->data, event->shared->size);
        break;
    case EVENT_CALL:
        if (event->shared) {
            event->callback(worker->hub, event->shared->data, event->shared->size);
        } else {
            event->callback(worker->hub, event->data, event->size);
        }
        break;
    case EVENT_EXIT:
        worker->exit = true;
        nw_loop_break();
        break;
    }
    event_drop(event);
}

static void on_wakeup(struct ev_loop *loop, ev_async *watcher, int events)
{
    struct nw_hub_worker *worker = watcher->data;
    struct event *event = worker_take(worker);
    while (event) {
        struct event *next = event->next;
        worker_handle(worker, event);
        event = next;
    }
}

static void worker_set_open(struct nw_hub_worker *worker, bool open)
{
    pthread_mutex_lock(&worker->lock);
    worker->open = open;
    pthread_mutex_unlock(&worker->lock);
}

static void worker_svr_release(struct nw_hub_worker *worker)
{
    nw_hub *hub = worker->hub;
    nw_svr *svr = worker->svr;
    worker->svr = NULL;
    if (hub->type.on_svr_create) {
        hub->type.on_svr_release(hub, worker->index, svr);
    } else {
        nw_svr_release(svr);
    }
}

/* tell nw_hub_create how the start of this worker went */
static void worker_report(struct nw_hub_worker *worker, int state)
{
    nw_hub *hub = worker->hub;
    pthread_mutex_lock(&hub->lock);
    worker->state = state;
    pthread_cond_broadcast(&hub->cond);
    pthread_mutex_unlock(&hub->lock);
}

static void *worker_main(void *arg)
{
    struct nw_hub_worker *worker = arg;
    nw_hub *hub = worker->hub;
    current = worker;

    /*
     * a complete nw_svr with the same binds, which is never started: it
     * does not listen, connections are put into it by the hub thread
     * (EVENT_ADD_FD). that way a connection of a hub is a connection of a
     * nw_svr in every respect. the price is one unused socket per bind.
     */
    nw_loop_init();
    worker->loop = nw_default_loop;
    if (hub->type.on_svr_create) {
        worker->svr = hub->type.on_svr_create(hub, worker->index);
    } else {
        worker->svr = nw_svr_create(&hub->svr_cfg, &hub->type.svr, hub->privdata);
    }
    if (worker->svr == NULL) {
        nw_loop_release();
        worker_report(worker, WORKER_FAILED);
        return NULL;
    }
    worker->svr->id_start = ((uint64_t)worker->index << ID_SHIFT) + 1;
    ev_async_init(&worker->wakeup, on_wakeup);
    worker->wakeup.data = worker;
    ev_async_start(worker->loop, &worker->wakeup);

    if (hub->type.on_thread_start && hub->type.on_thread_start(hub, worker->index) < 0) {
        ev_async_stop(worker->loop, &worker->wakeup);
        worker_svr_release(worker);
        nw_loop_release();
        worker_report(worker, WORKER_FAILED);
        return NULL;
    }
    worker_set_open(worker, true);
    worker_report(worker, WORKER_READY);

    /* a callback that calls nw_loop_break by mistake does not end the worker */
    while (!worker->exit) {
        nw_loop_run();
    }

    /* close the connections, then let the application clean up. other
     * workers may still be running and sending here: from now on they are
     * told no */
    worker_set_open(worker, false);
    nw_svr_iterator iter;
    nw_ses *ses;
    nw_svr_iter_init(&iter, worker->svr);
    while ((ses = nw_svr_next(&iter)) != NULL) {
        nw_svr_close_clt(worker->svr, ses);
    }
    if (hub->type.on_thread_stop) {
        hub->type.on_thread_stop(hub, worker->index);
    }

    ev_async_stop(worker->loop, &worker->wakeup);
    worker_svr_release(worker);
    nw_loop_release();
    return NULL;
}

/* on the hub thread: a connection was accepted, pass it on */
static int on_accept(nw_ses *ses, int sockfd, nw_addr *peer_addr)
{
    nw_hub *hub = nw_hub_of(ses);
    struct event *event = event_new(EVENT_ADD_FD, sizeof(nw_addr));
    if (event == NULL)
        return -1;
    event->fd = sockfd;
    event->bind_index = ses - hub->acceptor->svr_list;
    memcpy(event->data, peer_addr, sizeof(nw_addr));

    struct nw_hub_worker *worker = &hub->workers[hub->turn++ % hub->thread_count];
    if (worker_post(worker, event) < 0) {
        /* the caller closes sockfd */
        event->fd = -1;
        event_drop(event);
        return -1;
    }
    return 0;
}

/* stop and join the workers that run, free everything. also used on a
 * half built hub */
static void hub_free(nw_hub *hub)
{
    if (hub->workers) {
        /* tell all of them, then wait for all of them: while one is still
         * running it may send to any other, so nothing is taken apart
         * before the last one has ended */
        for (uint32_t i = 0; i < hub->thread_count; ++i) {
            struct nw_hub_worker *worker = &hub->workers[i];
            if (!worker->thread_created || worker->state != WORKER_READY)
                continue;
            struct event *event;
            /* this one has to get through */
            while ((event = event_new(EVENT_EXIT, 0)) == NULL)
                usleep(1000);
            if (worker_post(worker, event) < 0)
                event_drop(event);
        }
        for (uint32_t i = 0; i < hub->thread_count; ++i) {
            struct nw_hub_worker *worker = &hub->workers[i];
            if (worker->thread_created)
                pthread_join(worker->thread, NULL);
        }
        for (uint32_t i = 0; i < hub->thread_count; ++i) {
            struct nw_hub_worker *worker = &hub->workers[i];
            /* what came too late */
            struct event *event = worker_take(worker);
            while (event) {
                struct event *next = event->next;
                event_drop(event);
                event = next;
            }
            pthread_mutex_destroy(&worker->lock);
        }
        free(hub->workers);
    }
    if (hub->acceptor)
        nw_svr_release(hub->acceptor);
    free(hub->svr_cfg.bind_arr);
    pthread_cond_destroy(&hub->cond);
    pthread_mutex_destroy(&hub->lock);
    free(hub);
}

nw_hub *nw_hub_create(nw_hub_cfg *cfg, nw_hub_type *type, void *privdata)
{
    if (cfg->thread_count == 0 || cfg->thread_count > NW_HUB_MAX_THREADS)
        return NULL;
    if (cfg->svr.bind_count == 0)
        return NULL;
    if (type->on_svr_create && !type->on_svr_release)
        return NULL;
    for (uint32_t i = 0; i < cfg->svr.bind_count; ++i) {
        if (cfg->svr.bind_arr[i].sock_type == SOCK_DGRAM)
            return NULL;
    }

    nw_hub *hub = malloc(sizeof(nw_hub));
    if (hub == NULL)
        return NULL;
    memset(hub, 0, sizeof(nw_hub));
    pthread_mutex_init(&hub->lock, NULL);
    pthread_cond_init(&hub->cond, NULL);
    hub->type = *type;
    hub->type.svr.on_accept = NULL;
    hub->svr_cfg = cfg->svr;
    hub->svr_cfg.bind_arr = NULL;
    hub->queue_limit = cfg->queue_limit;
    hub->privdata = privdata;

    /* the workers read the binds after this function has returned */
    hub->svr_cfg.bind_arr = malloc(sizeof(nw_svr_bind) * cfg->svr.bind_count);
    if (hub->svr_cfg.bind_arr == NULL) {
        hub_free(hub);
        return NULL;
    }
    memcpy(hub->svr_cfg.bind_arr, cfg->svr.bind_arr, sizeof(nw_svr_bind) * cfg->svr.bind_count);

    hub->workers = calloc(cfg->thread_count, sizeof(struct nw_hub_worker));
    if (hub->workers == NULL) {
        hub_free(hub);
        return NULL;
    }
    hub->thread_count = cfg->thread_count;
    for (uint32_t i = 0; i < hub->thread_count; ++i) {
        struct nw_hub_worker *worker = &hub->workers[i];
        worker->hub = hub;
        worker->index = i;
        pthread_mutex_init(&worker->lock, NULL);
    }

    /* on this thread: a nw_svr that only accepts */
    nw_svr_type acceptor_type = hub->type.svr;
    acceptor_type.on_accept = on_accept;
    hub->acceptor = nw_svr_create(&hub->svr_cfg, &acceptor_type, hub->privdata);
    if (hub->acceptor == NULL) {
        hub_free(hub);
        return NULL;
    }

    /* signals are for the threads of the application, not for the workers */
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    bool failed = false;
    for (uint32_t i = 0; i < hub->thread_count; ++i) {
        struct nw_hub_worker *worker = &hub->workers[i];
        if (pthread_create(&worker->thread, NULL, worker_main, worker) != 0) {
            failed = true;
            break;
        }
        worker->thread_created = true;
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    /* wait until every thread that was started says how it went */
    pthread_mutex_lock(&hub->lock);
    for (uint32_t i = 0; i < hub->thread_count; ++i) {
        struct nw_hub_worker *worker = &hub->workers[i];
        if (!worker->thread_created)
            continue;
        while (worker->state == WORKER_STARTING)
            pthread_cond_wait(&hub->cond, &hub->lock);
        if (worker->state == WORKER_FAILED)
            failed = true;
    }
    pthread_mutex_unlock(&hub->lock);

    if (failed) {
        hub_free(hub);
        return NULL;
    }
    hub->next = hubs_here;
    hubs_here = hub;
    return hub;
}

int nw_hub_start(nw_hub *hub)
{
    return nw_svr_start(hub->acceptor);
}

int nw_hub_stop(nw_hub *hub)
{
    return nw_svr_stop(hub->acceptor);
}

void nw_hub_release(nw_hub *hub)
{
    for (nw_hub **link = &hubs_here; *link; link = &(*link)->next) {
        if (*link == hub) {
            *link = hub->next;
            break;
        }
    }
    nw_svr_stop(hub->acceptor);
    hub_free(hub);
}

uint32_t nw_hub_index_of(uint64_t id)
{
    return id >> ID_SHIFT;
}

/* the worker of a connection id. *local says whether that is the calling
 * thread, which then acts directly instead of asking itself */
static struct nw_hub_worker *worker_of(nw_hub *hub, uint64_t id, bool *local)
{
    uint32_t index = nw_hub_index_of(id);
    if (id == 0 || index >= hub->thread_count)
        return NULL;
    struct nw_hub_worker *worker = &hub->workers[index];
    *local = (worker == current);
    return worker;
}

int nw_hub_send(nw_hub *hub, uint64_t id, const void *data, size_t size)
{
    bool local;
    struct nw_hub_worker *worker = worker_of(hub, id, &local);
    if (worker == NULL)
        return -1;
    if (local)
        return nw_svr_send(worker->svr, id, data, size);

    struct event *event = event_new(EVENT_SEND, size);
    if (event == NULL)
        return -1;
    event->id = id;
    memcpy(event->data, data, size);
    if (worker_post(worker, event) < 0) {
        event_drop(event);
        return -1;
    }
    return 0;
}

static int close_by_id(nw_hub *hub, uint64_t id, int type)
{
    bool local;
    struct nw_hub_worker *worker = worker_of(hub, id, &local);
    if (worker == NULL)
        return -1;
    if (local) {
        nw_ses *ses = nw_svr_get_clt(worker->svr, id);
        if (ses == NULL)
            return 0;
        if (type == EVENT_CLOSE) {
            nw_svr_close_clt(worker->svr, ses);
        } else {
            nw_svr_shutdown_clt(worker->svr, ses);
        }
        return 0;
    }

    struct event *event = event_new(type, 0);
    if (event == NULL)
        return -1;
    event->id = id;
    if (worker_post(worker, event) < 0) {
        event_drop(event);
        return -1;
    }
    return 0;
}

int nw_hub_close(nw_hub *hub, uint64_t id)
{
    return close_by_id(hub, id, EVENT_CLOSE);
}

int nw_hub_shutdown(nw_hub *hub, uint64_t id)
{
    return close_by_id(hub, id, EVENT_SHUTDOWN);
}

int nw_hub_broadcast(nw_hub *hub, const void *data, size_t size)
{
    int ret = 0;
    struct shared_data *shared = NULL;

    for (uint32_t i = 0; i < hub->thread_count; ++i) {
        struct nw_hub_worker *worker = &hub->workers[i];
        if (worker == current)
            continue;
        if (shared == NULL) {
            shared = malloc(sizeof(struct shared_data) + size);
            if (shared == NULL) {
                ret = -1;
                break;
            }
            /* one reference is ours until the loop is done */
            shared->refs = 1;
            shared->size = size;
            memcpy(shared->data, data, size);
        }
        struct event *event = event_new(EVENT_BROADCAST, 0);
        if (event == NULL) {
            ret = -1;
            continue;
        }
        __sync_add_and_fetch(&shared->refs, 1);
        event->shared = shared;
        if (worker_post(worker, event) < 0) {
            event_drop(event);
            ret = -1;
        }
    }
    if (shared)
        shared_unref(shared);

    /* the connections of this thread, if it is a worker of this hub */
    if (current && current->hub == hub)
        broadcast_here(current->svr, data, size);
    return ret;
}

int nw_hub_post(nw_hub *hub, uint32_t index, nw_hub_callback callback, const void *data, size_t size)
{
    if (index >= hub->thread_count)
        return -1;
    struct nw_hub_worker *worker = &hub->workers[index];
    if (worker == current) {
        /* a copy, as promised: the callback may do with it what it likes */
        void *copy = NULL;
        if (size > 0) {
            copy = malloc(size);
            if (copy == NULL)
                return -1;
            memcpy(copy, data, size);
        }
        callback(hub, copy, size);
        free(copy);
        return 0;
    }

    struct event *event = event_new(EVENT_CALL, size);
    if (event == NULL)
        return -1;
    event->callback = callback;
    if (size > 0)
        memcpy(event->data, data, size);
    if (worker_post(worker, event) < 0) {
        event_drop(event);
        return -1;
    }
    return 0;
}

int nw_hub_post_all(nw_hub *hub, nw_hub_callback callback, const void *data, size_t size)
{
    struct shared_data *shared = malloc(sizeof(struct shared_data) + size);
    if (shared == NULL)
        return -1;
    /* one reference is ours until the end */
    shared->refs = 1;
    shared->size = size;
    if (size > 0)
        memcpy(shared->data, data, size);

    int ret = 0;
    for (uint32_t i = 0; i < hub->thread_count; ++i) {
        struct nw_hub_worker *worker = &hub->workers[i];
        if (worker == current)
            continue;
        struct event *event = event_new(EVENT_CALL, 0);
        if (event == NULL) {
            ret = -1;
            continue;
        }
        __sync_add_and_fetch(&shared->refs, 1);
        event->shared = shared;
        event->callback = callback;
        if (worker_post(worker, event) < 0) {
            event_drop(event);
            ret = -1;
        }
    }
    if (current && current->hub == hub)
        callback(hub, shared->data, shared->size);
    shared_unref(shared);
    return ret;
}

nw_hub *nw_hub_of(nw_ses *ses)
{
    /* a connection: its worker is the calling thread */
    if (current && current->svr == ses->svr)
        return current->hub;
    /* a listening session: the calling thread accepts for its hub */
    for (nw_hub *hub = hubs_here; hub; hub = hub->next) {
        if (hub->acceptor == ses->svr)
            return hub;
    }
    return NULL;
}

int nw_hub_index(nw_hub *hub)
{
    if (current && current->hub == hub)
        return current->index;
    return -1;
}

nw_svr *nw_hub_svr(nw_hub *hub)
{
    if (current && current->hub == hub)
        return current->svr;
    return NULL;
}
