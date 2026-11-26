/*
 * nw_clt on_close: sending from it fails quietly instead of recursing
 * (regression: it used to call on_close again without end), nw_clt_stop
 * from it means "stay closed", and a connect attempt that gets no answer
 * is given up after the timeout and retried.
 */
# include "common.h"

static nw_clt *clt;
static int close_count, connect_ok, connect_fail;
static bool stay_closed;

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
}

static void on_connect(nw_ses *ses, bool result)
{
    if (result)
        connect_ok++;
    else
        connect_fail++;
}

static int on_close(nw_ses *ses)
{
    close_count++;
    CHECK_EQ(nw_ses_send(ses, "last words\n", 11), -1);     /* the connection is over */
    if (stay_closed)
        CHECK_EQ(nw_clt_stop(clt), 0);
    return 0;
}

static int listener(int backlog, char *cfg, size_t cfg_size)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(in);
    CHECK_EQ(bind(fd, (struct sockaddr *)&in, len), 0);
    CHECK_EQ(listen(fd, backlog), 0);
    CHECK_EQ(getsockname(fd, (struct sockaddr *)&in, &len), 0);
    snprintf(cfg, cfg_size, "tcp@127.0.0.1:%d", ntohs(in.sin_port));
    return fd;
}

/* accept the client's connection and reset it */
static void reset_peer(int lfd)
{
    int fd = accept(lfd, NULL, NULL);
    CHECK(fd >= 0);
    struct linger lg = { 1, 0 };
    CHECK_EQ(setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)), 0);
    close(fd);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    char cfg[64];
    int lfd = listener(16, cfg, sizeof(cfg));

    nw_clt_type ct;
    memset(&ct, 0, sizeof(ct));
    ct.decode_pkg = ut_line_decode;
    ct.on_recv_pkg = on_recv_pkg;
    ct.on_connect = on_connect;
    ct.on_close = on_close;
    clt = ut_clt(cfg, &ct, 1024, 0.1, NULL);
    CHECK_EQ(nw_clt_start(clt), 0);
    ut_run_for(0.05);
    CHECK_EQ(connect_ok, 1);

    /* the server resets the connection: on_close once, then a reconnect */
    reset_peer(lfd);
    ut_run_for(0.05);
    CHECK_EQ(close_count, 1);
    CHECK(!nw_clt_connected(clt));
    ut_run_for(0.3);
    CHECK_EQ(connect_ok, 2);
    CHECK(nw_clt_connected(clt));
    CHECK_EQ(nw_ses_send(&clt->ses, "works again\n", 12), 0);

    /* nw_clt_stop from the application: on_close once, no reconnect */
    CHECK_EQ(nw_clt_stop(clt), 0);
    CHECK_EQ(close_count, 2);
    ut_run_for(0.4);
    CHECK_EQ(connect_ok, 2);
    CHECK(!nw_clt_connected(clt));

    /* nw_clt_stop from inside on_close: the lost connection is not
     * replaced */
    CHECK_EQ(nw_clt_start(clt), 0);
    ut_run_for(0.05);
    CHECK_EQ(connect_ok, 3);
    stay_closed = true;
    reset_peer(lfd);        /* the connection that was closed by nw_clt_stop above */
    reset_peer(lfd);        /* the current one */
    ut_run_for(0.5);
    CHECK_EQ(close_count, 3);
    CHECK_EQ(connect_ok, 3);                            /* it stayed closed */
    CHECK(!nw_timer_active(&clt->timer));
    stay_closed = false;
    nw_clt_release(clt);
    close(lfd);

    /*
     * a connect attempt that gets no answer. a listener with a full
     * backlog ignores further connection requests, so the client's attempt
     * just hangs; after reconnect_timeout it is given up (on_close) and
     * started again
     */
    lfd = listener(0, cfg, sizeof(cfg));
    int fillers[8];
    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    in.sin_port = htons(atoi(strrchr(cfg, ':') + 1));
    for (int i = 0; i < 8; ++i) {
        fillers[i] = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        connect(fillers[i], (struct sockaddr *)&in, sizeof(in));
    }
    usleep(50 * 1000);

    close_count = connect_ok = connect_fail = 0;
    clt = ut_clt(cfg, &ct, 1024, 0.2, NULL);
    CHECK_EQ(nw_clt_start(clt), 0);
    ut_run_for(0.9);
    fprintf(stderr, "hanging connect, 0.2s timeout, 0.9s: given up %d times, connected %d times\n", close_count, connect_ok);
    if (connect_ok == 0) {
        CHECK(close_count >= 3 && close_count <= 5);
        CHECK_EQ(connect_fail, 0);                      /* a timeout is reported by on_close only */
        CHECK(!nw_clt_connected(clt));
    } else {
        /* this kernel let the connection through anyway: nothing to check */
        fprintf(stderr, "the backlog did not fill up here, connect timeout not exercised\n");
    }
    nw_clt_release(clt);
    for (int i = 0; i < 8; ++i)
        close(fillers[i]);
    close(lfd);

    return 0;
}
