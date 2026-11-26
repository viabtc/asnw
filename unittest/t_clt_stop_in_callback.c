/*
 * nw_clt_stop and nw_clt_start called from the callbacks of the client
 * other than on_close: the callback has taken over, and the library does
 * not go on with the connection it was reporting (regression: it called
 * on_close a second time and reconnected, so that a client stopped from
 * on_connect or on_error_msg never stopped).
 */
# include <sys/socket.h>
# include <netinet/in.h>
# include "common.h"

static nw_clt *clt;
static int connect_ok, connect_fail, close_count, error_count;
/* what the callbacks do */
static bool stop_on_fail, stop_on_error, stop_on_connect;
static int restarts_left;

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
}

static void on_connect(nw_ses *ses, bool result)
{
    if (result) {
        connect_ok++;
        if (stop_on_connect)
            CHECK_EQ(nw_clt_stop(clt), 0);
        return;
    }
    connect_fail++;
    if (restarts_left > 0) {
        restarts_left--;
        CHECK_EQ(nw_clt_start(clt), 0);
    } else if (stop_on_fail) {
        CHECK_EQ(nw_clt_stop(clt), 0);
    }
}

static int on_close(nw_ses *ses)
{
    close_count++;
    return 0;
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    error_count++;
    if (stop_on_error)
        CHECK_EQ(nw_clt_stop(clt), 0);
}

static int listener(char *cfg, size_t cfg_size)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(in);
    CHECK_EQ(bind(fd, (struct sockaddr *)&in, len), 0);
    CHECK_EQ(listen(fd, 16), 0);
    CHECK_EQ(getsockname(fd, (struct sockaddr *)&in, &len), 0);
    snprintf(cfg, cfg_size, "tcp@127.0.0.1:%d", ntohs(in.sin_port));
    return fd;
}

/* accept the client's connection and reset it: an error, not a clean end */
static void reset_peer(int lfd)
{
    int fd = accept(lfd, NULL, NULL);
    CHECK(fd >= 0);
    struct linger lg = { 1, 0 };
    CHECK_EQ(setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)), 0);
    close(fd);
}

static void quiet(void)
{
    /* stopped: no timer waits to connect again, and nothing happens */
    CHECK(!nw_timer_active(&clt->timer));
    CHECK(!nw_clt_connected(clt));
    int fails = connect_fail, closes = close_count, oks = connect_ok;
    ut_run_for(0.4);
    CHECK_EQ(connect_fail, fails);
    CHECK_EQ(close_count, closes);
    CHECK_EQ(connect_ok, oks);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    char cfg[64], dead[64];
    int lfd = listener(cfg, sizeof(cfg));
    /* nobody listens there */
    close(listener(dead, sizeof(dead)));

    nw_clt_type ct;
    memset(&ct, 0, sizeof(ct));
    ct.decode_pkg = ut_line_decode;
    ct.on_recv_pkg = on_recv_pkg;
    ct.on_connect = on_connect;
    ct.on_close = on_close;
    ct.on_error_msg = on_error_msg;

    /* stopped from on_connect(false): on_close once, and that is all */
    clt = ut_clt(dead, &ct, 1024, 0.05, NULL);
    stop_on_fail = true;
    CHECK_EQ(nw_clt_start(clt), 0);
    ut_run_for(0.2);
    CHECK_EQ(connect_fail, 1);
    CHECK_EQ(close_count, 1);
    quiet();
    /* a stopped client starts again when told to, and stops the same way */
    CHECK_EQ(nw_clt_start(clt), 0);
    ut_run_for(0.2);
    CHECK_EQ(connect_fail, 2);
    CHECK_EQ(close_count, 2);
    quiet();

    /* started again from on_connect(false): the new attempt is the one
     * that counts, the failed one is not closed a second time nor retried
     * by the library. three times, then stopped */
    connect_fail = close_count = 0;
    restarts_left = 3;
    CHECK_EQ(nw_clt_start(clt), 0);
    ut_run_for(0.3);
    CHECK_EQ(connect_fail, 4);
    CHECK_EQ(close_count, 1);           /* the one of nw_clt_stop */
    quiet();
    stop_on_fail = false;
    nw_clt_release(clt);

    /* stopped from on_error_msg, for a connection that broke */
    connect_ok = connect_fail = close_count = error_count = 0;
    clt = ut_clt(cfg, &ct, 1024, 0.05, NULL);
    CHECK_EQ(nw_clt_start(clt), 0);
    ut_run_for(0.05);
    CHECK_EQ(connect_ok, 1);
    stop_on_error = true;
    reset_peer(lfd);
    ut_run_for(0.2);
    fprintf(stderr, "a reset connection: on_error_msg %d, on_close %d\n", error_count, close_count);
    CHECK_EQ(close_count, 1);
    if (error_count > 0) {
        CHECK_EQ(error_count, 1);
        CHECK_EQ(connect_ok, 1);        /* not connected again */
        quiet();
    } else {
        /* the reset was seen as an ordinary end here: the client went on */
        CHECK(connect_ok >= 1);
    }
    stop_on_error = false;
    nw_clt_release(clt);

    /* stopped from on_connect(true): connected, closed, not connected again */
    connect_ok = connect_fail = close_count = error_count = 0;
    clt = ut_clt(cfg, &ct, 1024, 0.05, NULL);
    stop_on_connect = true;
    CHECK_EQ(nw_clt_start(clt), 0);
    ut_run_for(0.2);
    CHECK_EQ(connect_ok, 1);
    CHECK_EQ(close_count, 1);
    quiet();
    nw_clt_release(clt);

    close(lfd);
    nw_loop_release();
    return 0;
}
