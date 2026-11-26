/*
 * Description: thread pool, runs blocking work off the loop thread
 *     History: yang@haipo.me, 2016/04/17, create
 */

# include <stdlib.h>
# include <string.h>
# include <signal.h>

# include "nw_job.h"

struct thread_arg {
    nw_job *job;
    void *privdata;
};

static void *thread_routine(void *data)
{
    struct thread_arg *arg = data;
    nw_job *job = arg->job;
    void *privdata = arg->privdata;
    free(data);

    for (;;) {
        pthread_mutex_lock(&job->request_lock);
        while (job->request_head == NULL && !job->shutdown) {
            pthread_cond_wait(&job->notify, &job->request_lock);
        }
        if (job->shutdown) {
            pthread_mutex_unlock(&job->request_lock);
            break;
        }
        nw_job_entry *entry = job->request_head;
        job->request_head = entry->next;
        if (job->request_head == NULL)
            job->request_tail = NULL;
        pthread_mutex_unlock(&job->request_lock);

        job->type.on_job(entry, privdata);

        entry->next = NULL;
        pthread_mutex_lock(&job->result_lock);
        if (job->result_tail) {
            job->result_tail->next = entry;
        } else {
            job->result_head = entry;
        }
        job->result_tail = entry;
        pthread_mutex_unlock(&job->result_lock);
        /* wake the loop thread. several of these before it gets there
         * amount to one wake-up */
        ev_async_send(job->loop, &job->wakeup);
    }

    return privdata;
}

/* on the loop thread: deliver what is done */
static void on_wakeup(struct ev_loop *loop, ev_async *watcher, int events)
{
    nw_job *job = watcher->data;
    pthread_mutex_lock(&job->result_lock);
    nw_job_entry *entry = job->result_head;
    job->result_head = NULL;
    job->result_tail = NULL;
    pthread_mutex_unlock(&job->result_lock);

    nw_job_entry *done = entry;
    while (entry) {
        if (job->type.on_finish)
            job->type.on_finish(entry);
        if (job->type.on_cleanup)
            job->type.on_cleanup(entry);
        entry = entry->next;
    }

    /* the entries go back to the cache, which nw_job_add takes them from
     * on any thread: under the lock, and once for all of them */
    pthread_mutex_lock(&job->request_lock);
    while (done) {
        nw_job_entry *next = done->next;
        nw_cache_free(job->cache, done);
        done = next;
    }
    pthread_mutex_unlock(&job->request_lock);
}

static void nw_job_free(nw_job *job)
{
    pthread_mutex_destroy(&job->request_lock);
    pthread_mutex_destroy(&job->result_lock);
    pthread_cond_destroy(&job->notify);
    if (job->threads)
        free(job->threads);
    if (job->cache)
        nw_cache_release(job->cache);
    free(job);
}

/* jobs that will never be finished: let the user free request/result */
static void drop_entries(nw_job *job, nw_job_entry *entry)
{
    while (entry) {
        nw_job_entry *next = entry->next;
        if (job->type.on_cleanup)
            job->type.on_cleanup(entry);
        nw_cache_free(job->cache, entry);
        entry = next;
    }
}

nw_job *nw_job_create(nw_job_type *type, int thread_count)
{
    if (!type->on_job)
        return NULL;
    if (type->on_init && !type->on_release)
        return NULL;
    if (thread_count <= 0)
        return NULL;

    nw_job *job = malloc(sizeof(nw_job));
    if (job == NULL)
        return NULL;
    memset(job, 0, sizeof(nw_job));
    nw_loop_init();
    job->type = *type;
    job->loop = nw_default_loop;
    pthread_mutex_init(&job->request_lock, NULL);
    pthread_mutex_init(&job->result_lock, NULL);
    pthread_cond_init(&job->notify, NULL);
    job->thread_count = thread_count;
    job->threads = calloc(job->thread_count, sizeof(pthread_t));
    if (job->threads == NULL) {
        nw_job_free(job);
        return NULL;
    }
    job->cache = nw_cache_create(sizeof(nw_job_entry));
    if (job->cache == NULL) {
        nw_job_free(job);
        return NULL;
    }
    ev_async_init(&job->wakeup, on_wakeup);
    job->wakeup.data = job;
    ev_async_start(job->loop, &job->wakeup);

    /* signals are for the threads of the application, not for the workers */
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    bool failed = false;
    for (int i = 0; i < job->thread_count; ++i) {
        struct thread_arg *arg = malloc(sizeof(struct thread_arg));
        if (arg == NULL) {
            failed = true;
            break;
        }
        memset(arg, 0, sizeof(struct thread_arg));
        arg->job = job;
        if (job->type.on_init) {
            arg->privdata = job->type.on_init();
            if (arg->privdata == NULL) {
                free(arg);
                failed = true;
                break;
            }
        }
        if (pthread_create(&job->threads[i], NULL, thread_routine, arg) != 0) {
            if (arg->privdata)
                job->type.on_release(arg->privdata);
            free(arg);
            failed = true;
            break;
        }
        job->thread_start++;
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (failed) {
        nw_job_release(job);
        return NULL;
    }

    return job;
}

int nw_job_add(nw_job *job, uint64_t id, void *request)
{
    /* any thread may be here: the cache is used under the lock as well */
    pthread_mutex_lock(&job->request_lock);
    nw_job_entry *entry = nw_cache_alloc(job->cache);
    if (entry == NULL) {
        pthread_mutex_unlock(&job->request_lock);
        return -1;
    }
    memset(entry, 0, sizeof(nw_job_entry));
    entry->id = id;
    entry->request = request;
    if (job->request_tail) {
        job->request_tail->next = entry;
    } else {
        job->request_head = entry;
    }
    job->request_tail = entry;
    pthread_cond_signal(&job->notify);
    pthread_mutex_unlock(&job->request_lock);

    return 0;
}

void nw_job_release(nw_job *job)
{
    pthread_mutex_lock(&job->request_lock);
    job->shutdown = true;
    pthread_cond_broadcast(&job->notify);
    pthread_mutex_unlock(&job->request_lock);
    for (int i = 0; i < job->thread_start; ++i) {
        void *privdata = NULL;
        if (pthread_join(job->threads[i], &privdata) != 0) {
            continue;
        }
        if (privdata != NULL && privdata != PTHREAD_CANCELED) {
            job->type.on_release(privdata);
        }
    }
    ev_async_stop(job->loop, &job->wakeup);
    /* all workers are gone, no lock needed. requests still queued and
     * replies not yet delivered get on_cleanup (not on_finish) */
    drop_entries(job, job->request_head);
    drop_entries(job, job->result_head);
    job->request_head = job->request_tail = NULL;
    job->result_head = job->result_tail = NULL;
    nw_job_free(job);
}
