/*
 * Several threads, each with a loop of its own and its own server, client,
 * timers, state table, job pool and rpc pair, all running at the same
 * time. Nothing is shared between them. Run under ThreadSanitizer
 * (make check-tsan), which is what makes this test say something.
 */
# include <pthread.h>
# include "common.h"
# include "nw_job.h"
# include "ut_rpc_svr.h"
# include "ut_rpc_clt.h"

# define THREADS  4
# define MESSAGES 200

struct worker {
    int index;
    pthread_t tid;
    struct ev_loop *loop;
    nw_svr *svr;
    nw_clt *clt;
    nw_state *state;
    nw_job *job;
    rpc_svr *rsvr;
    rpc_clt *rclt;
    int svr_recv, clt_recv, ticks, state_timeouts, jobs_done, rpc_replies;
    int svr_wrong_thread;
    char addr_text[NW_HUMAN_ADDR_SIZE];
};

static __thread struct worker *self;

static void maybe_done(void)
{
    if (self->clt_recv == MESSAGES && self->state_timeouts == 10 && self->jobs_done == 20 &&
            self->rpc_replies == 50 && self->ticks >= 3)
        nw_loop_break();
}

/* echo server and client */
static void svr_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    struct worker *w = ((nw_svr *)ses->svr)->privdata;
    if (w != self)
        w->svr_wrong_thread++;
    w->svr_recv++;
    /* the per thread static buffer */
    CHECK(strcmp(nw_sock_human_addr(&ses->peer_addr), "") != 0);
    CHECK_EQ(nw_ses_send(ses, data, size), 0);
}

static void send_next(struct worker *w)
{
    char line[64];
    int len = snprintf(line, sizeof(line), "thread %d message %d\n", w->index, w->clt_recv);
    CHECK_EQ(nw_ses_send(&w->clt->ses, line, len), 0);
}

static void clt_on_connect(nw_ses *ses, bool result)
{
    CHECK(result);
    send_next(self);
}

static void clt_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    struct worker *w = self;
    char line[64];
    int len = snprintf(line, sizeof(line), "thread %d message %d\n", w->index, w->clt_recv);
    CHECK_EQ((int)size, len);
    CHECK(memcmp(data, line, len) == 0);
    w->clt_recv++;
    if (w->clt_recv < MESSAGES)
        send_next(w);
    maybe_done();
}

static void on_tick(nw_timer *timer, void *privdata)
{
    struct worker *w = privdata;
    CHECK(w == self);
    w->ticks++;
    maybe_done();
}

static void on_state_timeout(nw_state_entry *entry)
{
    self->state_timeouts++;
    maybe_done();
}

/* runs on the pool threads of this worker's nw_job */
static void on_job(nw_job_entry *entry, void *privdata)
{
    usleep(1000);
    entry->result = entry->request;
}

static void on_job_finish(nw_job_entry *entry)
{
    struct worker *w = entry->request;
    CHECK(w == self);
    w->jobs_done++;
    maybe_done();
}

/* rpc: big and small packets, so both ways of building one are used */
static void rpc_svr_on_recv_pkg(nw_ses *ses, rpc_pkg *pkg)
{
    rpc_pkg reply = *pkg;
    reply.pkg_type = RPC_PKG_TYPE_REPLY;
    CHECK_EQ(rpc_send(ses, &reply), 0);
}

static void rpc_request(struct worker *w)
{
    static __thread char body[20000];
    rpc_pkg pkg;
    memset(&pkg, 0, sizeof(pkg));
    pkg.pkg_type = RPC_PKG_TYPE_REQUEST;
    pkg.command = 100 + w->index;
    pkg.sequence = w->rpc_replies;
    pkg.body = body;
    pkg.body_size = (w->rpc_replies % 2) ? sizeof(body) : 10;
    memset(body, 'a' + w->index, sizeof(body));
    CHECK_EQ(rpc_clt_send(w->rclt, &pkg), 0);
}

static void rpc_clt_on_connect(nw_ses *ses, bool result)
{
    CHECK(result);
    rpc_request(self);
}

static void rpc_clt_on_recv_pkg(nw_ses *ses, rpc_pkg *pkg)
{
    struct worker *w = self;
    CHECK_EQ(pkg->command, (uint32_t)(100 + w->index));
    CHECK_EQ(pkg->sequence, (uint32_t)w->rpc_replies);
    CHECK_EQ(((char *)pkg->body)[pkg->body_size - 1], 'a' + w->index);
    w->rpc_replies++;
    if (w->rpc_replies < 50)
        rpc_request(w);
    maybe_done();
}

static void *run(void *arg)
{
    struct worker *w = self = arg;
    char cfg[64];

    /* a server of this thread and its client */
    nw_svr_bind bind;
    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1:0", &bind.addr, &bind.sock_type), 0);
    nw_svr_cfg scfg;
    memset(&scfg, 0, sizeof(scfg));
    scfg.bind_count = 1;
    scfg.bind_arr = &bind;
    scfg.max_pkg_size = 1024;
    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = ut_line_decode;
    st.on_recv_pkg = svr_on_recv_pkg;
    w->svr = nw_svr_create(&scfg, &st, w);
    CHECK(w->svr != NULL);
    CHECK_EQ(nw_svr_start(w->svr), 0);
    w->loop = nw_default_loop;
    CHECK(w->loop != NULL);

    nw_addr addr;
    CHECK_EQ(nw_sock_host_addr(w->svr->bind_list[0].sockfd, &addr), 0);
    snprintf(cfg, sizeof(cfg), "tcp@%s", nw_sock_human_addr(&addr));
    snprintf(w->addr_text, sizeof(w->addr_text), "%s", nw_sock_human_addr(&addr));

    nw_clt_cfg ccfg;
    memset(&ccfg, 0, sizeof(ccfg));
    CHECK_EQ(nw_sock_cfg_parse(cfg, &ccfg.addr, &ccfg.sock_type), 0);
    ccfg.max_pkg_size = 1024;
    nw_clt_type ct;
    memset(&ct, 0, sizeof(ct));
    ct.decode_pkg = ut_line_decode;
    ct.on_recv_pkg = clt_on_recv_pkg;
    ct.on_connect = clt_on_connect;
    w->clt = nw_clt_create(&ccfg, &ct, w);
    CHECK(w->clt != NULL);
    CHECK_EQ(nw_clt_start(w->clt), 0);

    /* a unix dgram client: its local path must not collide with the one
     * another thread makes at the same moment */
    nw_clt_cfg dcfg;
    memset(&dcfg, 0, sizeof(dcfg));
    CHECK_EQ(nw_sock_cfg_parse("dgram@/tmp/asnw-ut-nobody.sock", &dcfg.addr, &dcfg.sock_type), 0);
    dcfg.max_pkg_size = 1024;
    nw_clt *dclt = nw_clt_create(&dcfg, &ct, w);
    CHECK(dclt != NULL);
    nw_clt_start(dclt);

    nw_timer tick;
    nw_timer_set(&tick, 0.01, true, on_tick, w);
    nw_timer_start(&tick);

    nw_state_type stt;
    memset(&stt, 0, sizeof(stt));
    stt.on_timeout = on_state_timeout;
    w->state = nw_state_create(&stt, sizeof(int));
    CHECK(w->state != NULL);
    for (int i = 0; i < 10; ++i)
        CHECK(nw_state_add(w->state, 0.02 + i * 0.005, 0) != NULL);

    nw_job_type jt;
    memset(&jt, 0, sizeof(jt));
    jt.on_job = on_job;
    jt.on_finish = on_job_finish;
    w->job = nw_job_create(&jt, 2);
    CHECK(w->job != NULL);
    for (int i = 0; i < 20; ++i)
        CHECK_EQ(nw_job_add(w->job, i, w), 0);

    nw_svr_cfg rscfg;
    memset(&rscfg, 0, sizeof(rscfg));
    nw_svr_bind rbind;
    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1:0", &rbind.addr, &rbind.sock_type), 0);
    rscfg.bind_count = 1;
    rscfg.bind_arr = &rbind;
    rscfg.max_pkg_size = RPC_MAX_PKG_SIZE;
    rpc_svr_type rst;
    memset(&rst, 0, sizeof(rst));
    rst.on_recv_pkg = rpc_svr_on_recv_pkg;
    w->rsvr = rpc_svr_create(&rscfg, &rst);
    CHECK(w->rsvr != NULL);
    CHECK_EQ(rpc_svr_start(w->rsvr), 0);
    CHECK_EQ(nw_sock_host_addr(w->rsvr->raw_svr->bind_list[0].sockfd, &addr), 0);

    rpc_clt_cfg rccfg;
    memset(&rccfg, 0, sizeof(rccfg));
    char rname[32];
    snprintf(rname, sizeof(rname), "rpc%d", w->index);
    rccfg.name = rname;
    rccfg.addr_count = 1;
    rccfg.addr_arr = &addr;
    rccfg.sock_type = SOCK_STREAM;
    rpc_clt_type rct;
    memset(&rct, 0, sizeof(rct));
    rct.on_connect = rpc_clt_on_connect;
    rct.on_recv_pkg = rpc_clt_on_recv_pkg;
    w->rclt = rpc_clt_create(&rccfg, &rct);
    CHECK(w->rclt != NULL);
    CHECK_EQ(rpc_clt_start(w->rclt), 0);

    /* give up after 20 seconds */
    nw_timer giveup;
    nw_timer_set(&giveup, 20, false, ut_on_stop, NULL);
    nw_timer_start(&giveup);

    nw_loop_run();

    nw_timer_stop(&giveup);
    nw_timer_stop(&tick);
    nw_clt_release(dclt);
    rpc_clt_release(w->rclt);
    rpc_svr_release(w->rsvr);
    nw_job_release(w->job);
    nw_state_release(w->state);
    nw_clt_release(w->clt);
    nw_svr_release(w->svr);

    /* the loop goes away with the thread, and can be made again */
    nw_loop_release();
    CHECK(nw_default_loop == NULL);
    nw_loop_init();
    CHECK(nw_default_loop != NULL);
    nw_loop_release();
    return NULL;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    static struct worker workers[THREADS];

    /* the main thread has a loop too, which the others leave alone */
    nw_loop_init();
    struct ev_loop *main_loop = nw_default_loop;

    for (int i = 0; i < THREADS; ++i) {
        workers[i].index = i;
        CHECK_EQ(pthread_create(&workers[i].tid, NULL, run, &workers[i]), 0);
    }
    for (int i = 0; i < THREADS; ++i)
        CHECK_EQ(pthread_join(workers[i].tid, NULL), 0);

    for (int i = 0; i < THREADS; ++i) {
        struct worker *w = &workers[i];
        fprintf(stderr, "thread %d on %s: echo %d/%d, ticks %d, states %d, jobs %d, rpc %d\n", i, w->addr_text,
                w->svr_recv, w->clt_recv, w->ticks, w->state_timeouts, w->jobs_done, w->rpc_replies);
        CHECK_EQ(w->svr_recv, MESSAGES);
        CHECK_EQ(w->clt_recv, MESSAGES);
        CHECK_EQ(w->state_timeouts, 10);
        CHECK_EQ(w->jobs_done, 20);
        CHECK_EQ(w->rpc_replies, 50);
        CHECK(w->ticks >= 3);
        CHECK_EQ(w->svr_wrong_thread, 0);           /* callbacks ran on the owner's thread */
        CHECK(w->loop != main_loop);
        for (int j = 0; j < i; ++j) {
            CHECK(w->loop != workers[j].loop);      /* a loop each */
            CHECK(strcmp(w->addr_text, workers[j].addr_text) != 0);
        }
    }

    CHECK(nw_default_loop == main_loop);
    ut_run_for(0.01);                               /* and the main loop still works */
    nw_loop_release();
    return 0;
}
