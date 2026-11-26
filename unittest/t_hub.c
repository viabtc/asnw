/*
 * nw_hub: connections spread over worker threads, the callbacks running
 * on the worker of their connection, and requests crossing threads by id.
 * The hub thread (main) also runs the clients. Says most under
 * ThreadSanitizer (make check-tsan).
 */
# include <pthread.h>
# include <sys/socket.h>
# include "common.h"
# include "nw_hub.h"

# define THREADS 4
# define CLIENTS 16
# define RELAYS  20
# define BIG     1024

static nw_hub *hub;

/* written by the workers */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t ids[256];
static int id_count;
static int per_thread[THREADS], unix_conns, closes, wrong_thread;
static int privdata_allocs, privdata_frees, thread_starts, thread_stops, timeouts;
static int sleeping, wake_up;
static int bye_on_close;
static int fail_start_at = -1;

# define ADD(var) __sync_fetch_and_add(&(var), 1)
# define GET(var) __sync_fetch_and_add(&(var), 0)

/* every callback of a connection must run on the worker its id names */
static void check_thread(nw_ses *ses)
{
    nw_hub *h = nw_hub_of(ses);
    if (h != hub || nw_hub_index(h) != (int)nw_hub_index_of(ses->id) || nw_hub_svr(h) != ses->svr)
        ADD(wrong_thread);
    /* the privdata of the hub is the privdata of every nw_svr in it */
    if (((nw_svr *)ses->svr)->privdata != &hub)
        ADD(wrong_thread);
}

static void *on_privdata_alloc(void *svr)
{
    ADD(privdata_allocs);
    return calloc(1, 16);
}

static void on_privdata_free(void *svr, void *privdata)
{
    ADD(privdata_frees);
    free(privdata);
}

static void on_open(nw_ses *ses)
{
    check_thread(ses);
    if (ses->privdata == NULL)
        ADD(wrong_thread);
    ADD(per_thread[nw_hub_index(hub)]);
    if (ses->local_addr->family == AF_UNIX)
        ADD(unix_conns);
    pthread_mutex_lock(&lock);
    ids[id_count++] = ses->id;
    pthread_mutex_unlock(&lock);
}

static void on_close(nw_ses *ses)
{
    check_thread(ses);
    ADD(closes);
    /* during the release: some of the other workers have ended already */
    if (GET(bye_on_close))
        nw_hub_broadcast(hub, "bye\n", 4);
}

static void on_timeout(nw_ses *ses)
{
    check_thread(ses);
    ADD(timeouts);
    nw_svr_close(ses->svr, ses);
}

static void svr_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    check_thread(ses);
    char line[256];
    char *text = data;
    text[size - 1] = 0;

    if (strncmp(text, "echo", 4) == 0) {
        int len = snprintf(line, sizeof(line), "echo %llu %d\n", (unsigned long long)ses->id, nw_hub_index(hub));
        CHECK_EQ(nw_ses_send(ses, line, len), 0);
    } else if (strcmp(text, "relay") == 0) {
        /* to everybody by id: the own connection, others of this worker,
         * and those of the other workers */
        uint64_t copy[256];
        pthread_mutex_lock(&lock);
        int count = id_count;
        memcpy(copy, ids, sizeof(uint64_t) * count);
        pthread_mutex_unlock(&lock);
        for (int i = 0; i < count; ++i)
            CHECK_EQ(nw_hub_send(hub, copy[i], "relay\n", 6), 0);
    } else if (strcmp(text, "bcast") == 0) {
        CHECK_EQ(nw_hub_broadcast(hub, "wbcast\n", 7), 0);
    } else if (strcmp(text, "shutme") == 0) {
        /* by id on the own worker: carried out at once */
        uint64_t id = ses->id;
        CHECK_EQ(nw_hub_send(hub, id, "last\n", 5), 0);
        CHECK_EQ(nw_hub_shutdown(hub, id), 0);
        CHECK_EQ(nw_hub_send(hub, id, "x\n", 2), -1);
    } else if (strcmp(text, "closeme") == 0) {
        uint64_t id = ses->id;
        CHECK_EQ(nw_hub_close(hub, id), 0);
        CHECK(nw_svr_get(nw_hub_svr(hub), id) == NULL);
        CHECK_EQ(nw_hub_close(hub, id), 0);
        CHECK_EQ(nw_hub_send(hub, id, "x\n", 2), -1);
    } else if (strcmp(text, "sleep") == 0) {
        /* stuck until the main thread says so */
        ADD(sleeping);
        while (GET(wake_up) == 0)
            usleep(1000);
    }
}

/* nw_hub_run: runs on the worker asked for, with the data given */
static int posted[THREADS], posted_all, posted_inline;

static void on_post(nw_hub *h, void *data, size_t size)
{
    int *want = data;
    if (h != hub || size != sizeof(int) || nw_hub_index(h) != *want || nw_hub_svr(h) == NULL)
        ADD(wrong_thread);
    ADD(posted[*want]);
}

static void on_post_all(nw_hub *h, void *data, size_t size)
{
    if (size != 5 || memcmp(data, "hello", 5) != 0 || nw_hub_index(h) < 0)
        ADD(wrong_thread);
    ADD(posted_all);
}

static void on_post_empty(nw_hub *h, void *data, size_t size)
{
    if (size != 0)
        ADD(wrong_thread);
    /* a worker asking itself is served at once */
    int before = GET(posted[nw_hub_index(h)]);
    int me = nw_hub_index(h);
    if (nw_hub_run(h, me, on_post, &me, sizeof(me)) != 0 || GET(posted[me]) != before + 1)
        ADD(wrong_thread);
    nw_hub_run_all(h, on_post_all, "hello", 5);
    ADD(posted_inline);
}

static int on_thread_start(nw_hub *h, uint32_t index)
{
    ADD(thread_starts);
    if (nw_hub_index(h) != (int)index || nw_hub_svr(h) == NULL || nw_default_loop == NULL)
        ADD(wrong_thread);
    return (int)index == fail_start_at ? -1 : 0;
}

static void on_thread_stop(nw_hub *h, uint32_t index)
{
    ADD(thread_stops);
    if (nw_hub_index(h) != (int)index || nw_hub_svr(h)->ses_count != 0)
        ADD(wrong_thread);
}

/* the clients, all on the main thread */
struct client {
    nw_clt *clt;
    uint64_t id;            /* its id on the server, from the echo */
    int worker;
    int echo, direct, news, relay, wbcast, last, big, closed;
    bool last_before_close;
};

static struct client clients[CLIENTS];

static void clt_on_connect(nw_ses *ses, bool result)
{
    CHECK(result);
    CHECK_EQ(nw_ses_send(ses, "echo\n", 5), 0);
}

static int clt_on_close(nw_ses *ses)
{
    struct client *c = ses->privdata;
    c->closed++;
    c->last_before_close = c->last == 1;
    nw_clt_stop(c->clt);       /* stay closed */
    return 0;
}

static void clt_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    struct client *c = ses->privdata;
    char *text = data;
    text[size - 1] = 0;
    unsigned long long id;
    if (sscanf(text, "echo %llu %d", &id, &c->worker) == 2) {
        c->id = id;
        c->echo++;
    } else if (strcmp(text, "direct") == 0) {
        c->direct++;
    } else if (strcmp(text, "news") == 0) {
        c->news++;
    } else if (strcmp(text, "relay") == 0) {
        c->relay++;
    } else if (strcmp(text, "wbcast") == 0) {
        c->wbcast++;
    } else if (strcmp(text, "last") == 0) {
        c->last++;
    } else if (strcmp(text, "bye") == 0) {
        /* any number of them */
    } else if (size == BIG) {
        c->big++;
    } else {
        fprintf(stderr, "unexpected: %s\n", text);
        exit(1);
    }
}

static nw_hub *create(uint32_t threads, nw_svr_bind *binds, uint32_t bind_count, double idle_timeout, size_t queue_limit)
{
    nw_hub_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.svr.bind_count = bind_count;
    cfg.svr.binds = binds;
    cfg.svr.max_pkg_size = 4096;
    cfg.svr.idle_timeout = idle_timeout;
    cfg.thread_count = threads;
    cfg.queue_limit = queue_limit;
    nw_hub_type type;
    memset(&type, 0, sizeof(type));
    type.svr.decode_pkg = ut_line_decode;
    type.svr.on_recv_pkg = svr_on_recv_pkg;
    type.svr.on_open = on_open;
    type.svr.on_close = on_close;
    type.svr.on_privdata_alloc = on_privdata_alloc;
    type.svr.on_privdata_free = on_privdata_free;
    if (idle_timeout > 0)
        type.svr.on_timeout = on_timeout;
    type.on_thread_start = on_thread_start;
    type.on_thread_stop = on_thread_stop;
    return nw_hub_create(&cfg, &type, &hub);
}

static bool all_have(int offset, int expect)
{
    for (int i = 0; i < CLIENTS; ++i) {
        struct client *c = &clients[i];
        if (!c->closed && *(int *)((char *)c + offset) != expect)
            return false;
    }
    return true;
}

/* run the loop until cond holds, 20 seconds at most: the machine may be
 * slow (valgrind, tsan, a busy machine). the checks come afterwards */
# define WAIT(cond) do { \
    for (int _i = 0; _i < 1000 && !(cond); ++_i) \
        ut_run_for(0.02); \
} while (0)
# define ALL(field, expect) all_have(offsetof(struct client, field), expect)

static void each_client(const char *what, int offset, int expect)
{
    for (int i = 0; i < CLIENTS; ++i) {
        struct client *c = &clients[i];
        if (c->closed)
            continue;
        int got = *(int *)((char *)c + offset);
        if (got != expect) {
            fprintf(stderr, "client %d: %s = %d, expected %d\n", i, what, got, expect);
            exit(1);
        }
    }
}
# define EACH(field, expect) each_client(#field, offsetof(struct client, field), expect)

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    nw_svr_bind binds[2];
    char unix_cfg[128], tcp_cfg[64];
    ut_unix_cfg(unix_cfg, sizeof(unix_cfg), "stream", "hub.sock");
    CHECK_EQ(nw_sock_addr_parse("tcp@127.0.0.1:0", &binds[0].addr, &binds[0].sock_type), 0);
    CHECK_EQ(nw_sock_addr_parse(unix_cfg, &binds[1].addr, &binds[1].sock_type), 0);

    /* what can't be */
    CHECK(create(0, binds, 2, 0, 0) == NULL);
    CHECK(create(NW_HUB_MAX_THREADS + 1, binds, 2, 0, 0) == NULL);
    nw_svr_bind dgram;
    CHECK_EQ(nw_sock_addr_parse("udp@127.0.0.1:0", &dgram.addr, &dgram.sock_type), 0);
    CHECK(create(2, &dgram, 1, 0, 0) == NULL);
    CHECK_EQ(GET(thread_starts), 0);

    /* one worker fails to start: no hub, the others are stopped again */
    fail_start_at = 2;
    CHECK(create(THREADS, binds, 2, 0, 0) == NULL);
    CHECK_EQ(GET(thread_starts), THREADS);
    CHECK_EQ(GET(thread_stops), THREADS - 1);
    fail_start_at = -1;
    thread_starts = thread_stops = 0;

    hub = create(THREADS, binds, 2, 0, 64 * BIG);
    CHECK(hub != NULL);
    CHECK_EQ(GET(thread_starts), THREADS);
    CHECK(hub->privdata == &hub);
    CHECK_EQ(nw_hub_start(hub), 0);
    CHECK_EQ(nw_hub_index(hub), -1);        /* this is not a worker */
    CHECK(nw_hub_svr(hub) == NULL);
    CHECK(nw_hub_of(&hub->acceptor->binds[1]) == hub);
    CHECK(hub->acceptor->privdata == &hub);

    /* a function of ours on a worker, and on all of them */
    for (int i = 0; i < THREADS; ++i)
        CHECK_EQ(nw_hub_run(hub, i, on_post, &i, sizeof(i)), 0);
    CHECK_EQ(nw_hub_run(hub, THREADS, on_post, NULL, 0), -1);
    CHECK_EQ(nw_hub_run_all(hub, on_post_all, "hello", 5), 0);
    CHECK_EQ(nw_hub_run(hub, 1, on_post_empty, NULL, 0), 0);
    for (int i = 0; i < 1000 && GET(posted_all) < 2 * THREADS; ++i)
        usleep(1000);
    for (int i = 0; i < THREADS; ++i)
        CHECK_EQ(GET(posted[i]), i == 1 ? 2 : 1);
    CHECK_EQ(GET(posted_all), 2 * THREADS);
    CHECK_EQ(GET(posted_inline), 1);
    snprintf(tcp_cfg, sizeof(tcp_cfg), "tcp@127.0.0.1:%d", ut_svr_port(hub->acceptor, 0));

    nw_clt_type ct;
    memset(&ct, 0, sizeof(ct));
    ct.decode_pkg = ut_line_decode;
    ct.on_recv_pkg = clt_on_recv_pkg;
    ct.on_connect = clt_on_connect;
    ct.on_close = clt_on_close;
    for (int i = 0; i < CLIENTS; ++i) {
        clients[i].clt = ut_clt(i % 2 ? unix_cfg : tcp_cfg, &ct, 4096, 10, &clients[i]);
        CHECK_EQ(nw_clt_start(clients[i].clt), 0);
    }
    WAIT(ALL(echo, 1));

    /* dealt out in turn, each bind keeps its address, ids tell the worker */
    EACH(echo, 1);
    CHECK_EQ(GET(unix_conns), CLIENTS / 2);
    for (int i = 0; i < THREADS; ++i)
        CHECK_EQ(GET(per_thread[i]), CLIENTS / THREADS);
    for (int i = 0; i < CLIENTS; ++i) {
        CHECK(clients[i].id != 0);
        CHECK_EQ(nw_hub_index_of(clients[i].id), clients[i].worker);
        for (int j = 0; j < i; ++j)
            CHECK(clients[i].id != clients[j].id);
    }

    /* from the hub thread, by id and to all */
    for (int i = 0; i < CLIENTS; ++i)
        CHECK_EQ(nw_hub_send(hub, clients[i].id, "direct\n", 7), 0);
    CHECK_EQ(nw_hub_broadcast(hub, "news\n", 5), 0);
    WAIT(ALL(direct, 1) && ALL(news, 1));
    ut_run_for(0.05);
    EACH(direct, 1);
    EACH(news, 1);

    /* from the workers, all at the same time */
    for (int n = 0; n < RELAYS; ++n) {
        for (int i = 0; i < CLIENTS; ++i)
            CHECK_EQ(nw_ses_send(&clients[i].clt->ses, "relay\n", 6), 0);
    }
    CHECK_EQ(nw_ses_send(&clients[1].clt->ses, "bcast\n", 6), 0);
    WAIT(ALL(relay, RELAYS * CLIENTS) && ALL(wbcast, 1));
    ut_run_for(0.05);
    EACH(relay, RELAYS * CLIENTS);
    EACH(wbcast, 1);

    /* ids that are not, or no longer */
    CHECK_EQ(nw_hub_send(hub, 0, "x\n", 2), -1);
    CHECK_EQ(nw_hub_send(hub, (uint64_t)THREADS << 48 | 1, "x\n", 2), -1);
    CHECK_EQ(nw_hub_close(hub, (uint64_t)99 << 48 | 1), -1);
    CHECK_EQ(nw_hub_send(hub, clients[0].id + 1000, "x\n", 2), 0);       /* queued, then dropped */
    CHECK_EQ(nw_hub_close(hub, clients[0].id + 1000), 0);

    /* a last message, then an orderly close; and a plain close */
    CHECK_EQ(nw_hub_send(hub, clients[3].id, "last\n", 5), 0);
    CHECK_EQ(nw_hub_shutdown(hub, clients[3].id), 0);
    CHECK_EQ(nw_hub_close(hub, clients[4].id), 0);
    WAIT(clients[3].closed && clients[4].closed && GET(closes) == 2);
    CHECK_EQ(clients[3].closed, 1);
    CHECK(clients[3].last_before_close);
    CHECK_EQ(clients[4].closed, 1);
    CHECK_EQ(GET(closes), 2);

    /* the same asked for by the connection's own worker */
    CHECK_EQ(nw_ses_send(&clients[6].clt->ses, "shutme\n", 7), 0);
    CHECK_EQ(nw_ses_send(&clients[7].clt->ses, "closeme\n", 8), 0);
    WAIT(clients[6].closed && clients[7].closed && GET(closes) == 4);
    CHECK_EQ(clients[6].closed, 1);
    CHECK(clients[6].last_before_close);
    CHECK_EQ(clients[7].closed, 1);
    CHECK_EQ(GET(closes), 4);

    /* a worker that is stuck: what waits for it is limited */
    struct client *slow = &clients[2];
    CHECK_EQ(nw_ses_send(&slow->clt->ses, "sleep\n", 6), 0);
    WAIT(GET(sleeping) == 1);
    CHECK_EQ(GET(sleeping), 1);
    char big[BIG];
    memset(big, 'b', sizeof(big));
    big[BIG - 1] = '\n';
    int queued = 0;
    while (queued < 1000 && nw_hub_send(hub, slow->id, big, BIG) == 0)
        queued++;
    CHECK_EQ(queued, 64);
    CHECK_EQ(nw_hub_broadcast(hub, "news\n", 5), -1);       /* the others get it */
    ADD(wake_up);
    WAIT(slow->big == 64 && clients[5].news == 2);
    ut_run_for(0.05);
    CHECK_EQ(slow->big, 64);
    CHECK_EQ(slow->news, 1);
    CHECK_EQ(clients[5].news, 2);
    CHECK_EQ(nw_hub_send(hub, slow->id, big, BIG), 0);      /* room again */
    WAIT(slow->big == 65);
    CHECK_EQ(slow->big, 65);

    /* release closes what is left, each on its worker */
    ADD(bye_on_close);
    nw_hub_release(hub);
    bye_on_close = 0;
    CHECK_EQ(GET(closes), CLIENTS);
    CHECK_EQ(GET(privdata_allocs), CLIENTS);
    CHECK_EQ(GET(privdata_frees), CLIENTS);
    CHECK_EQ(GET(thread_stops), THREADS);
    WAIT(ALL(closed, 1));
    for (int i = 0; i < CLIENTS; ++i) {
        CHECK_EQ(clients[i].closed, 1);
        nw_clt_release(clients[i].clt);
    }

    /* the idle timeout works on the workers like on any nw_svr */
    closes = 0;
    hub = create(2, binds, 1, 0.2, 0);
    CHECK(hub != NULL);
    CHECK_EQ(nw_hub_start(hub), 0);
    nw_addr addr;
    CHECK_EQ(nw_sock_local_addr(hub->acceptor->binds[0].sockfd, &addr), 0);
    int fds[4];
    for (int i = 0; i < 4; ++i) {
        fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        CHECK_EQ(connect(fds[i], NW_SOCKADDR(&addr), sizeof(addr.in)), 0);
    }
    WAIT(GET(timeouts) == 4 && GET(closes) == 4);
    CHECK_EQ(GET(timeouts), 4);
    CHECK_EQ(GET(closes), 4);
    /* stopped: nothing new is accepted, releasing still works */
    CHECK_EQ(nw_hub_stop(hub), 0);
    nw_hub_release(hub);
    for (int i = 0; i < 4; ++i)
        close(fds[i]);

    CHECK_EQ(GET(wrong_thread), 0);
    nw_loop_release();
    return 0;
}
