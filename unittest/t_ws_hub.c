/*
 * nw_ws on several threads (thread_count in the cfg): connections spread
 * over the workers, callbacks on the worker of the connection, and the
 * functions by id and the broadcasts working from any thread. The main
 * thread accepts and runs the test clients. Run under ThreadSanitizer.
 */
# include <pthread.h>
# include <sys/socket.h>
# include "common.h"
# include "nw_ws.h"

# define BIG_SIZE (100 * 1024)
# define THREADS  3
# define CLIENTS  9
# define ROUNDS   30

static nw_addr addr;
# include "ws_client.h"

static nw_ws_svr *front;
static int marker;
static int opens, closes, wrong, allocs, frees, thread_starts, thread_stops;
static int fail_start_at = -1;

# define ADD(var) __sync_fetch_and_add(&(var), 1)
# define GET(var) __sync_fetch_and_add(&(var), 0)

/* a callback of a connection runs on the worker its id names, with the
 * server of that worker */
static void check_thread(nw_ses *ses)
{
    nw_ws_svr *svr = nw_ws_svr_of(ses);
    if (svr == front || svr->front != front || svr->hub != front->hub || svr->privdata != &marker)
        ADD(wrong);
    if (svr->raw_svr != ses->svr || nw_hub_index(svr->hub) != (int)nw_hub_index_of(ses->id))
        ADD(wrong);
}

static void *on_privdata_alloc(void *svr)
{
    ADD(allocs);
    return calloc(1, 8);
}

static void on_privdata_free(void *svr, void *privdata)
{
    ADD(frees);
    free(privdata);
}

static void on_open(nw_ses *ses)
{
    check_thread(ses);
    if (nw_ws_privdata(ses) == NULL)
        ADD(wrong);
    ADD(opens);
}

static void on_close(nw_ses *ses)
{
    check_thread(ses);
    ADD(closes);
}

static void on_message(nw_ses *ses, bool binary, void *data, size_t size)
{
    check_thread(ses);
    nw_ws_svr *svr = nw_ws_svr_of(ses);
    char *text = data;
    char reply[128];
    unsigned long long id;
    int used = 0;

    if (strcmp(text, "id") == 0) {
        snprintf(reply, sizeof(reply), "id %llu %d", (unsigned long long)ses->id, nw_hub_index(svr->hub));
        CHECK_EQ(nw_ws_send_text(ses, reply), 0);
    } else if (strncmp(text, "all ", 4) == 0) {
        /* from a worker, with the server of that worker */
        CHECK_EQ(nw_ws_broadcast_text(svr, text + 4), 0);
    } else if (sscanf(text, "to %llu %n", &id, &used) == 1 && used > 0) {
        CHECK_EQ(nw_ws_send_text_id(svr, id, text + used), 0);
    } else if (sscanf(text, "kick %llu", &id) == 1) {
        CHECK_EQ(nw_ws_close_id(svr, id, NW_WS_CLOSE_POLICY, "kicked"), 0);
    }
}

static int on_thread_start(nw_ws_svr *svr, uint32_t index)
{
    ADD(thread_starts);
    if (svr->front != front && front != NULL)
        ADD(wrong);
    if (nw_hub_index(svr->hub) != (int)index || svr->raw_svr != nw_hub_svr(svr->hub))
        ADD(wrong);
    return (int)index == fail_start_at ? -1 : 0;
}

static void on_thread_stop(nw_ws_svr *svr, uint32_t index)
{
    ADD(thread_stops);
    if (svr->raw_svr->clt_count != 0)
        ADD(wrong);
}

static nw_ws_svr *create(uint32_t threads)
{
    static nw_svr_bind bind;
    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1:0", &bind.addr, &bind.sock_type), 0);
    nw_ws_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.bind_arr = &bind;
    cfg.max_message_size = BIG_SIZE;
    cfg.thread_count = threads;
    nw_ws_svr_type type;
    memset(&type, 0, sizeof(type));
    type.on_open = on_open;
    type.on_message = on_message;
    type.on_close = on_close;
    type.on_privdata_alloc = on_privdata_alloc;
    type.on_privdata_free = on_privdata_free;
    type.on_thread_start = on_thread_start;
    type.on_thread_stop = on_thread_stop;
    nw_ws_svr *svr = nw_ws_svr_create(&cfg, &type, &marker);
    if (svr == NULL)
        return NULL;
    CHECK(svr->hub != NULL);
    CHECK(svr->front == NULL);
    CHECK_EQ(nw_ws_svr_start(svr), 0);
    CHECK_EQ(nw_sock_host_addr(svr->raw_svr->bind_list[0].sockfd, &addr), 0);
    return svr;
}

struct member {
    struct client *c;
    unsigned long long id;
    int thread;
};

static struct member members[CLIENTS];

static void join(struct member *m)
{
    struct frame f;
    m->c = ws_connect("/");
    ws_send_text(m->c, "id");
    CHECK(ws_read(m->c, &f, 5.0));
    CHECK_EQ(sscanf((char *)f.data, "id %llu %d", &m->id, &m->thread), 2);
    CHECK_EQ(nw_hub_index_of(m->id), m->thread);
}

/* two connections on different threads, and two on the same */
static void pick(struct member **a, struct member **other_thread, struct member **same_thread)
{
    *a = &members[0];
    *other_thread = *same_thread = NULL;
    for (int i = 1; i < CLIENTS; ++i) {
        if (members[i].thread != members[0].thread && *other_thread == NULL)
            *other_thread = &members[i];
        if (members[i].thread == members[0].thread && *same_thread == NULL)
            *same_thread = &members[i];
    }
    CHECK(*other_thread != NULL && *same_thread != NULL);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    char text[256];
    struct frame f;

    /* a worker that fails to start: no server */
    fail_start_at = 1;
    CHECK(create(THREADS) == NULL);
    CHECK_EQ(GET(thread_starts), THREADS);
    CHECK_EQ(GET(thread_stops), THREADS - 1);
    fail_start_at = -1;
    thread_starts = thread_stops = 0;

    front = create(THREADS);
    CHECK(front != NULL);
    CHECK_EQ(GET(thread_starts), THREADS);

    /* dealt out in turn */
    int per_thread[THREADS] = { 0 };
    for (int i = 0; i < CLIENTS; ++i) {
        join(&members[i]);
        per_thread[members[i].thread]++;
    }
    for (int i = 0; i < THREADS; ++i)
        CHECK_EQ(per_thread[i], CLIENTS / THREADS);
    CHECK_EQ(GET(opens), CLIENTS);

    /* a connection that has not done the handshake gets no message */
    struct client *raw = client_new();
    client_pump(raw, 0.05);

    /* from the thread that accepts: by id, and to all */
    for (int i = 0; i < CLIENTS; ++i) {
        snprintf(text, sizeof(text), "hello %d", i);
        CHECK_EQ(nw_ws_send_text_id(front, members[i].id, text), 0);
    }
    CHECK_EQ(nw_ws_broadcast_text(front, "news"), 0);
    for (int i = 0; i < CLIENTS; ++i) {
        snprintf(text, sizeof(text), "hello %d", i);
        expect_text(members[i].c, text);
        expect_text(members[i].c, "news");
    }

    /* a big one, built once for all the threads */
    char *big = malloc(BIG_SIZE);
    for (int i = 0; i < BIG_SIZE; ++i)
        big[i] = (char)(i * 7);
    CHECK_EQ(nw_ws_broadcast_binary(front, big, BIG_SIZE), 0);
    for (int i = 0; i < CLIENTS; ++i) {
        CHECK(ws_read(members[i].c, &f, 5.0));
        CHECK_EQ(f.opcode, 2);
        CHECK_EQ(f.size, BIG_SIZE);
        CHECK(memcmp(f.data, big, BIG_SIZE) == 0);
    }
    free(big);
    client_pump(raw, 0.05);
    CHECK_EQ(raw->len, 0);
    CHECK(!raw->eof);
    client_free(raw);

    /* from a worker: to a connection of another thread and of its own */
    struct member *a, *far, *near;
    pick(&a, &far, &near);
    snprintf(text, sizeof(text), "to %llu over there", far->id);
    ws_send_text(a->c, text);
    expect_text(far->c, "over there");
    snprintf(text, sizeof(text), "to %llu right here", near->id);
    ws_send_text(a->c, text);
    expect_text(near->c, "right here");
    ws_send_text(far->c, "all from a worker");
    for (int i = 0; i < CLIENTS; ++i)
        expect_text(members[i].c, "from a worker");

    /* ids that are not, or no longer */
    CHECK_EQ(nw_ws_send_text_id(front, 0, "x"), -1);
    CHECK_EQ(nw_ws_send_text_id(front, (uint64_t)THREADS << 48 | 1, "x"), -1);
    CHECK_EQ(nw_ws_close_id(front, (uint64_t)50 << 48 | 1, NW_WS_CLOSE_NORMAL, NULL), -1);
    CHECK_EQ(nw_ws_send_text_id(front, a->id + 5000, "x"), 0);        /* queued, then dropped */
    CHECK_EQ(nw_ws_close_id(front, a->id + 5000, NW_WS_CLOSE_NORMAL, NULL), 0);

    /* closing by id: from the accepting thread, and from another worker */
    CHECK_EQ(nw_ws_close_id(front, members[CLIENTS - 1].id, NW_WS_CLOSE_GOING_AWAY, "bye"), 0);
    CHECK(ws_read(members[CLIENTS - 1].c, &f, 5.0));
    CHECK_EQ(f.opcode, 8);
    CHECK_EQ((f.data[0] << 8) | f.data[1], NW_WS_CLOSE_GOING_AWAY);
    CHECK_EQ(f.size, 5);
    CHECK(memcmp(f.data + 2, "bye", 3) == 0);
    CHECK(client_closed(members[CLIENTS - 1].c, 5.0));

    struct member *victim = &members[CLIENTS - 2];
    CHECK(victim != a);
    snprintf(text, sizeof(text), "kick %llu", victim->id);
    ws_send_text(victim->thread == a->thread ? far->c : a->c, text);
    expect_close(victim->c, NW_WS_CLOSE_POLICY);
    for (int i = 0; i < 500 && GET(closes) < 2; ++i)
        ut_run_for(0.01);
    CHECK_EQ(GET(closes), 2);

    /* everybody talks to everybody, all the threads broadcasting at once */
    int left = CLIENTS - 2;
    for (int n = 0; n < ROUNDS; ++n) {
        for (int i = 0; i < left; ++i) {
            snprintf(text, sizeof(text), "all round %d from %d", n, i);
            ws_send_text(members[i].c, text);
        }
    }
    for (int i = 0; i < left; ++i) {
        int got = 0, next_round[CLIENTS] = { 0 };
        while (got < ROUNDS * left) {
            int round, from;
            CHECK(ws_read(members[i].c, &f, 10.0));
            CHECK_EQ(sscanf((char *)f.data, "round %d from %d", &round, &from), 2);
            /* what one connection said arrives in the order it said it */
            CHECK_EQ(round, next_round[from]);
            next_round[from]++;
            got++;
        }
    }

    /* release closes the rest, on their threads */
    nw_ws_svr_release(front);
    CHECK_EQ(GET(closes), CLIENTS);
    CHECK_EQ(GET(allocs), CLIENTS);
    CHECK_EQ(GET(frees), CLIENTS);
    CHECK_EQ(GET(thread_stops), THREADS);
    for (int i = 0; i < left; ++i)
        CHECK(client_closed(members[i].c, 5.0) || members[i].c->eof);
    for (int i = 0; i < CLIENTS; ++i)
        client_free(members[i].c);

    /* one worker: the same server, off the thread that accepts */
    front = NULL;
    opens = closes = 0;
    front = create(1);
    CHECK(front != NULL);
    struct member one;
    join(&one);
    CHECK_EQ(one.thread, 0);
    CHECK_EQ(nw_ws_broadcast_text(front, "to the one"), 0);
    expect_text(one.c, "to the one");
    nw_ws_svr_release(front);
    CHECK_EQ(GET(closes), 1);
    client_free(one.c);

    CHECK_EQ(GET(wrong), 0);
    nw_loop_release();
    return 0;
}
