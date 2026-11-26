/*
 * Regression (fixed): a connection used to be still "live" while its own
 * on_connection_close ran. A callback that sent on it (the send fails, the
 * failure closes the connection, which calls the callback ...) or closed
 * another connection whose callback came back for this one recursed
 * without end, or released the session twice.
 * Now the connection is out of the server before the callback is called,
 * and walking the connections tolerates any of them being closed.
 */
# include "common.h"

# define N 40

static nw_svr *svr;
static nw_addr addr;
static int close_calls[N + 16];
static int total_closes;
static int cascade;         /* how many other connections a close callback closes */
static bool tell_others;    /* a close callback sends to everybody else */
static int timeout_calls, dead_timeouts;
static uint64_t first_id;

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
}

static void on_connection_close(nw_ses *ses)
{
    uint64_t id = ses->id;
    CHECK(id != 0);                                     /* still readable */
    CHECK(id >= first_id && id < first_id + N + 16);
    close_calls[id - first_id]++;
    total_closes++;

    /* the connection is already out of the server */
    CHECK(nw_svr_get_clt(svr, id) == NULL);
    CHECK_EQ(nw_ses_send(ses, "goodbye\n", 8), -1);
    CHECK_EQ(nw_svr_send(svr, id, "goodbye\n", 8), -1);
    nw_svr_close_clt(svr, ses);                         /* again: nothing happens */
    nw_svr_shutdown_clt(svr, ses);
    nw_svr_iterator iter;
    nw_ses *other;
    uint32_t seen = 0;
    nw_svr_iter_init(&iter, svr);
    while ((other = nw_svr_next(&iter)) != NULL) {
        CHECK(other != ses);
        CHECK(other->id != 0);
        seen++;
    }
    CHECK_EQ(seen, svr->clt_count);                     /* which no longer counts this one */

    if (tell_others) {
        nw_svr_iter_init(&iter, svr);
        while ((other = nw_svr_next(&iter)) != NULL)
            nw_ses_send(other, "somebody left\n", 14);
    }
    /* close some of the others from in here: their callbacks do the same */
    for (int i = 0; i < cascade; ++i) {
        nw_svr_iter_init(&iter, svr);
        other = nw_svr_next(&iter);
        if (other == NULL)
            break;
        nw_svr_close_clt(svr, other);
    }
}

static void on_timeout(nw_ses *ses)
{
    timeout_calls++;
    if (ses->id == 0) {
        dead_timeouts++;
        return;
    }
    nw_svr_close_clt(svr, ses);
}

static nw_svr *start(double idle_timeout)
{
    static nw_svr_bind bind;
    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1:0", &bind.addr, &bind.sock_type), 0);
    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.bind_arr = &bind;
    cfg.max_pkg_size = 1024;
    cfg.idle_timeout = idle_timeout;
    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = ut_line_decode;
    type.on_recv_pkg = on_recv_pkg;
    type.on_connection_close = on_connection_close;
    type.on_timeout = on_timeout;
    nw_svr *s = nw_svr_create(&cfg, &type, NULL);
    CHECK(s != NULL);
    CHECK_EQ(nw_svr_start(s), 0);
    CHECK_EQ(nw_sock_host_addr(s->svr_list[0].sockfd, &addr), 0);
    return s;
}

static void connect_many(int *fds, int count)
{
    memset(close_calls, 0, sizeof(close_calls));
    total_closes = 0;
    first_id = svr->id_start ? svr->id_start : 1;
    for (int i = 0; i < count; ++i) {
        fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(fds[i] >= 0);
        CHECK_EQ(connect(fds[i], NW_SOCKADDR(&addr), sizeof(addr.in)), 0);
    }
    ut_run_for(0.05);
    CHECK_EQ(svr->clt_count, count);
}

static void close_all(int *fds, int count)
{
    for (int i = 0; i < count; ++i)
        close(fds[i]);
    ut_run_for(0.05);
}

/* every connection got its callback exactly once */
static void check_each_closed_once(int count)
{
    CHECK_EQ(total_closes, count);
    for (int i = 0; i < count; ++i)
        CHECK_EQ(close_calls[i], 1);
    CHECK_EQ(svr->clt_count, 0);
    CHECK_EQ(nw_dict_count(svr->clt_dict), 0);
}

int main(void)
{
    int fds[N];
    signal(SIGPIPE, SIG_IGN);
    svr = start(0);

    /* a client resets its connection; the callback tries to send on it */
    connect_many(fds, 1);
    struct linger lg = { 1, 0 };
    CHECK_EQ(setsockopt(fds[0], SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)), 0);
    close(fds[0]);
    ut_run_for(0.1);
    check_each_closed_once(1);

    /* one client leaves; each callback closes two more: a chain reaction
     * through all of them, every one closed exactly once */
    cascade = 2;
    connect_many(fds, N);
    close(fds[7]);
    ut_run_for(0.1);
    check_each_closed_once(N);
    close_all(fds, N);

    /* the same started from the server side */
    connect_many(fds, N);
    nw_svr_close_clt(svr, nw_svr_get_clt(svr, first_id + 3));
    check_each_closed_once(N);
    close_all(fds, N);

    /* each callback tells all the others, some of which are broken: the
     * failed sends close those, from inside the walk */
    cascade = 0;
    tell_others = true;
    connect_many(fds, N);
    for (int i = 0; i < N; i += 2) {
        CHECK_EQ(setsockopt(fds[i], SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)), 0);
        close(fds[i]);
    }
    ut_run_for(0.2);
    CHECK_EQ(total_closes, N / 2);
    CHECK_EQ(svr->clt_count, N / 2);
    for (int i = 1; i < N; i += 2)
        close(fds[i]);
    ut_run_for(0.2);
    check_each_closed_once(N);
    tell_others = false;

    /* walking the connections and closing as we go, while every close
     * takes others with it: only live connections are returned */
    cascade = 3;
    connect_many(fds, N);
    nw_svr_iterator iter;
    nw_ses *ses;
    int visited = 0;
    nw_svr_iter_init(&iter, svr);
    while ((ses = nw_svr_next(&iter)) != NULL) {
        CHECK(ses->id != 0);
        CHECK(nw_svr_get_clt(svr, ses->id) == ses);
        visited++;
        nw_svr_close_clt(svr, ses);
    }
    CHECK(visited >= 1 && visited < N);
    check_each_closed_once(N);
    close_all(fds, N);

    /* release, with the callbacks closing each other */
    connect_many(fds, N);
    nw_svr_release(svr);
    CHECK_EQ(total_closes, N);
    for (int i = 0; i < N; ++i)
        CHECK_EQ(close_calls[i], 1);
    for (int i = 0; i < N; ++i)
        close(fds[i]);

    /* the timeout sweep: everybody times out at once and every close takes
     * others along. on_timeout is never called for a connection that is
     * already gone */
    svr = start(0.2);
    connect_many(fds, N);
    ut_run_for(0.6);
    fprintf(stderr, "%d connections timed out: on_timeout called %d times, %d on closed ones\n",
            N, timeout_calls, dead_timeouts);
    CHECK_EQ(dead_timeouts, 0);
    CHECK(timeout_calls >= 1 && timeout_calls < N);
    check_each_closed_once(N);
    close_all(fds, N);
    nw_svr_release(svr);

    return 0;
}
