/*
 * nw_http on several threads (thread_count in the cfg): requests handled
 * on the worker of their connection, answered there at once, later through
 * a nw_job of that thread, or from another thread by id. The main thread
 * accepts and runs the test clients. Run under ThreadSanitizer.
 */
# include <pthread.h>
# include <sys/socket.h>
# include "common.h"
# include "nw_http.h"

# define BIG_SIZE (256 * 1024)
# define THREADS  3
# define CLIENTS  9
# define ROUNDS   30

# include "http_client.h"

static nw_http_svr *front;
static nw_addr addr;
static int marker;
static int requests, closes, wrong, thread_starts, thread_stops, jobs_done;
static int fail_start_at = -1;

/* ids of requests waiting for an answer from the main thread */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t later_ids[16];
static int later_count;

/* one job pool per worker thread, made in on_thread_start */
static __thread nw_job *job;
/* and one of the main thread, which every worker adds jobs to */
static nw_job *shared_job;
static pthread_t main_thread;
static int shared_done;

# define ADD(var) __sync_fetch_and_add(&(var), 1)
# define GET(var) __sync_fetch_and_add(&(var), 0)

static void check_thread(nw_ses *ses)
{
    nw_http_svr *svr = nw_http_svr_of(ses);
    if (svr == front || svr->front != front || svr->hub != front->hub || svr->privdata != &marker)
        ADD(wrong);
    if (svr->raw_svr != ses->svr || nw_hub_index(svr->hub) != (int)nw_hub_index_of(ses->id))
        ADD(wrong);
}

struct work {
    nw_http_svr *svr;
    int value;
};

/* on a pool thread */
static void on_job(nw_job_entry *entry, void *privdata)
{
    struct work *work = entry->request;
    usleep(5000);
    work->value *= 2;
}

/* back on the worker thread of the connection: answering by id is local */
static void on_job_finish(nw_job_entry *entry)
{
    struct work *work = entry->request;
    char body[64];
    nw_http_response rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.status = 200;
    rsp.body = body;
    rsp.body_size = snprintf(body, sizeof(body), "job %d on %d", work->value, nw_hub_index(work->svr->hub));
    /* the id of the job is the id of the connection */
    if (nw_hub_index(work->svr->hub) != (int)nw_hub_index_of(entry->id))
        ADD(wrong);
    if (nw_http_reply_id(work->svr, entry->id, &rsp) != 0)
        ADD(wrong);
    ADD(jobs_done);
}

static void on_job_cleanup(nw_job_entry *entry)
{
    free(entry->request);
}

/* the shared pool: back on the main thread, the answer crosses over to the
 * worker of the connection */
static void on_shared_finish(nw_job_entry *entry)
{
    struct work *work = entry->request;
    if (!pthread_equal(pthread_self(), main_thread))
        ADD(wrong);
    char body[64];
    nw_http_response rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.status = 200;
    rsp.body = body;
    rsp.body_size = snprintf(body, sizeof(body), "shared %d", work->value);
    if (nw_http_reply_id(front, entry->id, &rsp) != 0)
        ADD(wrong);
    ADD(shared_done);
}

static void on_request(nw_ses *ses, nw_http_request *req)
{
    check_thread(ses);
    ADD(requests);
    nw_http_svr *svr = nw_http_svr_of(ses);
    char body[128];

    if (strcmp(req->path, "/where") == 0) {
        int len = snprintf(body, sizeof(body), "%llu %d", (unsigned long long)ses->id, nw_hub_index(svr->hub));
        CHECK_EQ(nw_http_reply_body(ses, 200, "text/plain", body, len), 0);
    } else if (strcmp(req->path, "/later") == 0) {
        /* the main thread answers */
        pthread_mutex_lock(&lock);
        later_ids[later_count++] = ses->id;
        pthread_mutex_unlock(&lock);
    } else if (strcmp(req->path, "/job") == 0) {
        struct work *work = malloc(sizeof(struct work));
        work->svr = svr;
        work->value = atoi(req->query);
        CHECK_EQ(nw_job_add(job, ses->id, work), 0);
    } else if (strcmp(req->path, "/shared") == 0) {
        struct work *work = malloc(sizeof(struct work));
        work->svr = svr;
        work->value = atoi(req->query);
        CHECK_EQ(nw_job_add(shared_job, ses->id, work), 0);
    } else if (strcmp(req->path, "/echo") == 0) {
        CHECK_EQ(nw_http_reply_body(ses, 200, "application/octet-stream", req->body, req->body_size), 0);
    } else {
        nw_http_reply_body(ses, 404, NULL, NULL, 0);
    }
}

static void on_connection_close(nw_ses *ses)
{
    check_thread(ses);
    ADD(closes);
}

static int on_thread_start(nw_http_svr *svr, uint32_t index)
{
    ADD(thread_starts);
    if (nw_hub_index(svr->hub) != (int)index || svr->raw_svr != nw_hub_svr(svr->hub))
        ADD(wrong);
    if ((int)index == fail_start_at)
        return -1;
    nw_job_type jt;
    memset(&jt, 0, sizeof(jt));
    jt.on_job = on_job;
    jt.on_finish = on_job_finish;
    jt.on_cleanup = on_job_cleanup;
    job = nw_job_create(&jt, 2);
    return job ? 0 : -1;
}

static void on_thread_stop(nw_http_svr *svr, uint32_t index)
{
    ADD(thread_stops);
    if (svr->raw_svr->clt_count != 0)
        ADD(wrong);
    if (job)
        nw_job_release(job);
    job = NULL;
}

static nw_http_svr *create(uint32_t threads)
{
    static nw_svr_bind bind;
    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1:0", &bind.addr, &bind.sock_type), 0);
    nw_http_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.bind_arr = &bind;
    cfg.max_request_size = BIG_SIZE + 4096;
    cfg.thread_count = threads;
    nw_http_svr_type type;
    memset(&type, 0, sizeof(type));
    type.on_request = on_request;
    type.on_connection_close = on_connection_close;
    type.on_thread_start = on_thread_start;
    type.on_thread_stop = on_thread_stop;
    nw_http_svr *svr = nw_http_svr_create(&cfg, &type, &marker);
    if (svr == NULL)
        return NULL;
    CHECK(svr->hub != NULL && svr->front == NULL);
    CHECK_EQ(nw_http_svr_start(svr), 0);
    CHECK_EQ(nw_sock_host_addr(svr->raw_svr->bind_list[0].sockfd, &addr), 0);
    addr.addrlen = sizeof(addr.in);
    return svr;
}

static void get(struct client *c, const char *target, struct response *rsp)
{
    char request[512];
    snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: test\r\n\r\n", target);
    client_puts(c, request);
    if (rsp)
        CHECK(client_read(c, rsp, false, 10.0));
}

static uint64_t take_later(void)
{
    uint64_t id = 0;
    for (int i = 0; i < 2000 && id == 0; ++i) {
        pthread_mutex_lock(&lock);
        if (later_count > 0)
            id = later_ids[--later_count];
        pthread_mutex_unlock(&lock);
        if (id == 0)
            ut_run_for(0.005);
    }
    CHECK(id != 0);
    return id;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    struct response rsp;
    nw_http_response answer;
    char text[256];

    /* a worker that fails to start: no server */
    fail_start_at = 1;
    CHECK(create(THREADS) == NULL);
    CHECK_EQ(GET(thread_starts), THREADS);
    CHECK_EQ(GET(thread_stops), THREADS - 1);
    fail_start_at = -1;
    thread_starts = thread_stops = 0;

    main_thread = pthread_self();
    nw_job_type shared_type;
    memset(&shared_type, 0, sizeof(shared_type));
    shared_type.on_job = on_job;
    shared_type.on_finish = on_shared_finish;
    shared_type.on_cleanup = on_job_cleanup;
    shared_job = nw_job_create(&shared_type, 3);
    CHECK(shared_job != NULL);

    front = create(THREADS);
    CHECK(front != NULL);
    CHECK_EQ(GET(thread_starts), THREADS);

    /* dealt out in turn, answered on the worker */
    struct client *clients[CLIENTS];
    unsigned long long ids[CLIENTS];
    int threads[CLIENTS], per_thread[THREADS] = { 0 };
    for (int i = 0; i < CLIENTS; ++i) {
        clients[i] = client_new(&addr);
        get(clients[i], "/where", &rsp);
        CHECK_EQ(rsp.status, 200);
        CHECK_EQ(sscanf(rsp.body, "%llu %d", &ids[i], &threads[i]), 2);
        CHECK_EQ(nw_hub_index_of(ids[i]), threads[i]);
        per_thread[threads[i]]++;
    }
    for (int i = 0; i < THREADS; ++i)
        CHECK_EQ(per_thread[i], CLIENTS / THREADS);

    /* answered later by the main thread: the whole response travels to the
     * worker, and the connection goes on afterwards */
    get(clients[0], "/later", NULL);
    uint64_t id = take_later();
    CHECK_EQ(id, ids[0]);
    memset(&answer, 0, sizeof(answer));
    answer.status = 201;
    answer.reason = "Made Elsewhere";
    nw_http_add_header(&answer, "X-From", "the main thread");
    nw_http_add_header(&answer, "Content-Type", "text/plain");
    answer.body = "crossed over";
    answer.body_size = 12;
    CHECK_EQ(nw_http_reply_id(front, id, &answer), 0);
    CHECK(client_read(clients[0], &rsp, false, 10.0));
    CHECK_EQ(rsp.status, 201);
    CHECK(strstr(rsp.head, "HTTP/1.1 201 Made Elsewhere\r\n") != NULL);
    CHECK(strstr(rsp.head, "X-From: the main thread\r\n") != NULL);
    CHECK(strstr(rsp.head, "Connection: keep-alive\r\n") != NULL);
    CHECK_EQ(rsp.body_size, 12);
    CHECK(memcmp(rsp.body, "crossed over", 12) == 0);
    /* answered already: a second response is dropped on the worker */
    CHECK_EQ(nw_http_reply_id(front, id, &answer), 0);
    get(clients[0], "/where", &rsp);
    CHECK_EQ(rsp.status, 200);
    CHECK_EQ(strtoull(rsp.body, NULL, 10), ids[0]);

    /* a big body, and a response that closes the connection */
    char *big = malloc(BIG_SIZE);
    for (int i = 0; i < BIG_SIZE; ++i)
        big[i] = (char)(i * 13);
    get(clients[1], "/later", NULL);
    id = take_later();
    memset(&answer, 0, sizeof(answer));
    answer.status = 200;
    answer.close = true;
    answer.body = big;
    answer.body_size = BIG_SIZE;
    CHECK_EQ(nw_http_reply_id(front, id, &answer), 0);
    CHECK(client_read(clients[1], &rsp, false, 10.0));
    CHECK_EQ(rsp.body_size, BIG_SIZE);
    CHECK(memcmp(rsp.body, big, BIG_SIZE) == 0);
    CHECK(strstr(rsp.head, "Connection: close\r\n") != NULL);
    CHECK(client_closed(clients[1], 5.0));

    /* ids that are not, or no longer */
    memset(&answer, 0, sizeof(answer));
    answer.status = 200;
    CHECK_EQ(nw_http_reply_id(front, 0, &answer), -1);
    CHECK_EQ(nw_http_reply_id(front, (uint64_t)THREADS << 48 | 1, &answer), -1);
    CHECK_EQ(nw_http_reply_id(front, ids[0] + 5000, &answer), 0);      /* queued, then dropped */

    /* the request body arrives in pieces after "100 Continue", which the
     * worker sends from a timer of its own loop */
    snprintf(text, sizeof(text), "POST /echo HTTP/1.1\r\nHost: test\r\nContent-Length: %d\r\nExpect: 100-continue\r\n\r\n", BIG_SIZE);
    client_puts(clients[2], text);
    CHECK(client_read(clients[2], &rsp, true, 10.0));
    CHECK_EQ(rsp.status, 100);
    for (int sent = 0; sent < BIG_SIZE; sent += 50000) {
        client_send(clients[2], big + sent, BIG_SIZE - sent < 50000 ? BIG_SIZE - sent : 50000);
        client_pump(clients[2], 0.002);
    }
    CHECK(client_read(clients[2], &rsp, false, 10.0));
    CHECK_EQ(rsp.status, 200);
    CHECK_EQ(rsp.body_size, BIG_SIZE);
    CHECK(memcmp(rsp.body, big, BIG_SIZE) == 0);
    free(big);

    /* everybody at once, round after round: answered at once, and through
     * the job pool of each worker */
    for (int n = 0; n < ROUNDS; ++n) {
        for (int i = 2; i < CLIENTS; ++i) {
            snprintf(text, sizeof(text), n % 2 ? "/job?%d" : "/where?%d", n * 100 + i);
            get(clients[i], text, NULL);
        }
        for (int i = 2; i < CLIENTS; ++i) {
            CHECK(client_read(clients[i], &rsp, false, 10.0));
            CHECK_EQ(rsp.status, 200);
            if (n % 2) {
                snprintf(text, sizeof(text), "job %d on %d", (n * 100 + i) * 2, threads[i]);
                CHECK(strcmp(rsp.body, text) == 0);
            } else {
                CHECK_EQ(strtoull(rsp.body, NULL, 10), ids[i]);
            }
        }
    }
    CHECK_EQ(GET(jobs_done), ROUNDS / 2 * (CLIENTS - 2));

    /* one pool for all the workers: they add the jobs, the main thread
     * gets the results and answers by id */
    for (int n = 0; n < ROUNDS; ++n) {
        for (int i = 2; i < CLIENTS; ++i) {
            snprintf(text, sizeof(text), "/shared?%d", n * 100 + i);
            get(clients[i], text, NULL);
        }
        for (int i = 2; i < CLIENTS; ++i) {
            CHECK(client_read(clients[i], &rsp, false, 10.0));
            CHECK_EQ(rsp.status, 200);
            snprintf(text, sizeof(text), "shared %d", (n * 100 + i) * 2);
            CHECK(strcmp(rsp.body, text) == 0);
        }
    }
    CHECK_EQ(GET(shared_done), ROUNDS * (CLIENTS - 2));

    /* a refused request is refused on the worker too */
    client_puts(clients[3], "BROKEN\r\n\r\n");
    CHECK(client_read(clients[3], &rsp, false, 10.0));
    CHECK_EQ(rsp.status, 400);
    CHECK(client_closed(clients[3], 5.0));

    /* release closes the rest, on their threads */
    for (int i = 0; i < 500 && GET(closes) < 2; ++i)
        ut_run_for(0.01);
    CHECK_EQ(GET(closes), 2);
    nw_http_svr_release(front);
    CHECK_EQ(GET(closes), CLIENTS);
    CHECK_EQ(GET(thread_stops), THREADS);
    for (int i = 0; i < CLIENTS; ++i)
        client_free(clients[i]);

    /* one worker */
    closes = 0;
    front = create(1);
    CHECK(front != NULL);
    struct client *one = client_new(&addr);
    get(one, "/where", &rsp);
    CHECK_EQ(rsp.status, 200);
    nw_http_svr_release(front);
    CHECK_EQ(GET(closes), 1);
    client_free(one);

    /* after the servers: no worker is left to add a job */
    nw_job_release(shared_job);
    CHECK_EQ(GET(wrong), 0);
    nw_loop_release();
    return 0;
}
