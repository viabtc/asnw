/*
 * nw_svr timeouts and orderly close: the idle timeout of the cfg, a
 * timeout or a deadline per connection, the on_timeout hook, and
 * nw_svr_shutdown_clt getting the last message through.
 */
# include "common.h"

# define BIG_SIZE (2 * 1024 * 1024)

static nw_svr *svr;
static nw_addr addr;
static char *big;
static int close_count, timeout_count, recv_count, error_count;
static double last_silence;
static bool keep_on_timeout;

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    char *s = data;
    recv_count++;
    if (strncmp(s, "bye", 3) == 0) {
        /* a last word, then an orderly close */
        CHECK_EQ(nw_ses_send(ses, "goodbye\n", 8), 0);
        nw_svr_shutdown_clt(svr, ses);
        CHECK(ses->closing);
        CHECK_EQ(nw_ses_send(ses, "too late\n", 9), -1);    /* nothing after that */
        nw_svr_shutdown_clt(svr, ses);                      /* twice is harmless */
    } else if (strncmp(s, "big", 3) == 0) {
        CHECK_EQ(nw_ses_send(ses, big, BIG_SIZE), 0);
        nw_svr_shutdown_clt(svr, ses);
    } else if (strncmp(s, "slow", 4) == 0) {
        nw_svr_set_clt_idle_timeout(svr, ses, 0);           /* this one may take its time */
    } else if (strncmp(s, "quick", 5) == 0) {
        nw_svr_set_clt_idle_timeout(svr, ses, 0.2);
    } else if (strncmp(s, "deadline", 8) == 0) {
        nw_svr_set_clt_deadline(svr, ses, 0.3);
    } else if (strncmp(s, "cancel", 6) == 0) {
        nw_svr_set_clt_deadline(svr, ses, 0);
    } else {
        nw_ses_send(ses, data, size);
    }
}

static void on_connection_close(nw_ses *ses)
{
    close_count++;
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    error_count++;
}

static void on_timeout(nw_ses *ses)
{
    timeout_count++;
    last_silence = ev_now(nw_default_loop) - ses->active_time;
    if (!keep_on_timeout) {
        nw_ses_send(ses, "timed out\n", 10);
        nw_svr_shutdown_clt(svr, ses);
    }
}

static int connect_to(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    CHECK_EQ(connect(fd, NW_SOCKADDR(&addr), sizeof(addr.in)), 0);
    return fd;
}

/* how the connection looks from the client side, without blocking:
 * 1 closed by the server, 0 still open */
static int is_closed(int fd)
{
    char buf[4096];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n == 0)
            return 1;
        if (n < 0)
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : 1;
    }
}

/* read everything the server sends until it closes, up to `timeout` */
static size_t read_all(int fd, char *out, size_t max, double timeout, bool *closed)
{
    size_t len = 0;
    *closed = false;
    for (double waited = 0; waited < timeout && !*closed; waited += 0.005) {
        ut_run_for(0.005);
        for (;;) {
            ssize_t n = recv(fd, out + len, max - len, MSG_DONTWAIT);
            if (n > 0) {
                len += n;
            } else {
                if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                    *closed = true;
                break;
            }
        }
    }
    return len;
}

static nw_svr *start(double idle_timeout, bool with_hook)
{
    static nw_svr_bind bind;
    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1:0", &bind.addr, &bind.sock_type), 0);
    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.bind_arr = &bind;
    cfg.max_pkg_size = 4096;
    cfg.idle_timeout = idle_timeout;
    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = ut_line_decode;
    type.on_recv_pkg = on_recv_pkg;
    type.on_connection_close = on_connection_close;
    type.on_error_msg = on_error_msg;
    if (with_hook)
        type.on_timeout = on_timeout;
    nw_svr *s = nw_svr_create(&cfg, &type, NULL);
    CHECK(s != NULL);
    CHECK_EQ(nw_svr_start(s), 0);
    CHECK_EQ(nw_sock_host_addr(s->svr_list[0].sockfd, &addr), 0);
    return s;
}

int main(void)
{
    /* the test's own writes go to connections the server may have closed */
    signal(SIGPIPE, SIG_IGN);

    char *buf = malloc(BIG_SIZE + 4096);
    bool closed;
    size_t len;
    big = malloc(BIG_SIZE);
    for (int i = 0; i < BIG_SIZE; ++i)
        big[i] = 'a' + i % 26;

    /*
     * no timeout configured: nothing times out, and no timer is running
     */
    svr = start(0, false);
    int fd = connect_to();
    ut_run_for(0.3);
    CHECK_EQ(svr->clt_count, 1);
    CHECK(!nw_timer_active(&svr->timer));
    CHECK_EQ(is_closed(fd), 0);

    /* an orderly close: the last message arrives, then the connection ends,
     * although the client keeps sending */
    CHECK_EQ(write(fd, "bye\nmore\nand more\n", 18), 18);
    len = read_all(fd, buf, 4096, 1.0, &closed);
    CHECK(closed);
    CHECK_EQ(len, 8);
    CHECK(memcmp(buf, "goodbye\n", 8) == 0);
    CHECK_EQ(recv_count, 1);                            /* "more" was thrown away */
    close(fd);
    ut_run_for(0.05);
    CHECK_EQ(svr->clt_count, 0);
    CHECK_EQ(close_count, 1);

    /* with far more queued than the socket takes at once: all of it is
     * written before the connection ends */
    fd = connect_to();
    CHECK_EQ(write(fd, "big\n", 4), 4);
    len = read_all(fd, buf, BIG_SIZE + 4096, 5.0, &closed);
    CHECK(closed);
    CHECK_EQ(len, BIG_SIZE);
    CHECK(memcmp(buf, big, BIG_SIZE) == 0);
    close(fd);

    /* a client that never hangs up is closed anyway after a short wait */
    fd = connect_to();
    CHECK_EQ(write(fd, "bye\n", 4), 4);
    ut_run_for(0.5);
    CHECK_EQ(svr->clt_count, 1);
    ut_run_for(2.6);
    CHECK_EQ(svr->clt_count, 0);
    close(fd);

    /* a deadline on one connection, set, cancelled, and set again */
    fd = connect_to();
    int other = connect_to();
    CHECK_EQ(write(fd, "deadline\ncancel\n", 16), 16);
    ut_run_for(0.8);
    CHECK_EQ(is_closed(fd), 0);                         /* it was cancelled */
    int errors = error_count;
    CHECK_EQ(write(fd, "deadline\n", 9), 9);
    for (int i = 0; i < 6; ++i) {                       /* being busy does not push it back */
        if (write(fd, "x\n", 2) < 0)
            break;
        ut_run_for(0.1);
    }
    CHECK_EQ(is_closed(fd), 1);
    CHECK_EQ(error_count, errors + 1);                  /* reported, then closed */
    CHECK_EQ(is_closed(other), 0);                      /* nobody else is affected */
    CHECK_EQ(svr->clt_count, 1);
    close(fd);
    close(other);
    ut_run_for(0.05);
    nw_svr_release(svr);

    /*
     * idle timeout from the cfg, default action: silent connections go,
     * talking ones stay
     */
    close_count = 0;
    svr = start(0.4, false);
    int silent = connect_to();
    int talking = connect_to();
    int patient = connect_to();
    int hasty = connect_to();
    CHECK_EQ(write(patient, "slow\n", 5), 5);          /* its timeout is turned off */
    CHECK_EQ(write(hasty, "quick\n", 6), 6);           /* its timeout is shortened */
    ut_run_for(0.3);
    CHECK_EQ(is_closed(hasty), 1);
    CHECK_EQ(is_closed(silent), 0);
    for (int i = 0; i < 10; ++i) {
        CHECK_EQ(write(talking, "ping\n", 5), 5);
        ut_run_for(0.1);
    }
    CHECK_EQ(is_closed(silent), 1);
    CHECK_EQ(is_closed(talking), 0);
    CHECK_EQ(is_closed(patient), 0);
    CHECK_EQ(svr->clt_count, 2);
    CHECK_EQ(close_count, 2);
    ut_run_for(0.7);                                    /* now talking falls silent too */
    CHECK_EQ(is_closed(talking), 1);
    CHECK_EQ(is_closed(patient), 0);
    close(silent);
    close(talking);
    close(patient);
    close(hasty);
    ut_run_for(0.05);
    /* with no connection left the timer stops, and starts again when needed */
    ut_run_for(0.3);
    CHECK_EQ(svr->clt_count, 0);
    CHECK(!nw_timer_active(&svr->timer));
    fd = connect_to();
    ut_run_for(0.05);
    CHECK(nw_timer_active(&svr->timer));
    ut_run_for(0.8);
    CHECK_EQ(is_closed(fd), 1);
    close(fd);
    nw_svr_release(svr);

    /*
     * on_timeout decides. here: say why, then close in an orderly way
     */
    svr = start(0.3, true);
    fd = connect_to();
    len = read_all(fd, buf, 4096, 2.0, &closed);
    CHECK(closed);
    CHECK_EQ(len, 10);
    CHECK(memcmp(buf, "timed out\n", 10) == 0);
    CHECK_EQ(timeout_count, 1);
    CHECK(last_silence >= 0.3 && last_silence < 0.7);
    close(fd);

    /* left open, a silent connection times out again every idle timeout,
     * and active_time tells how long the silence has lasted in total */
    keep_on_timeout = true;
    timeout_count = 0;
    fd = connect_to();
    ut_run_for(1.4);
    fprintf(stderr, "silent for 1.4s with a 0.3s timeout: fired %d times, last at %.2fs of silence\n",
            timeout_count, last_silence);
    CHECK(timeout_count >= 3 && timeout_count <= 4);
    CHECK(last_silence >= 0.9);
    CHECK_EQ(is_closed(fd), 0);
    /* data resets the count */
    CHECK_EQ(write(fd, "hello\n", 6), 6);
    ut_run_for(0.2);
    int fired = timeout_count;
    ut_run_for(0.3);
    CHECK_EQ(timeout_count, fired + 1);
    CHECK(last_silence < 0.6);
    close(fd);
    ut_run_for(0.05);
    nw_svr_release(svr);

    /*
     * the timer follows the connections of the moment: it speeds up for a
     * short timeout, slows down again when that one is gone, and stops
     * when nobody has a timeout left
     */
    keep_on_timeout = false;
    svr = start(30, false);
    int slow_one = connect_to();
    ut_run_for(0.05);
    CHECK(nw_timer_active(&svr->timer));
    CHECK(svr->timer_interval == 1.0);
    int quick_one = connect_to();
    CHECK_EQ(write(quick_one, "quick\n", 6), 6);         /* an idle timeout of 0.2s */
    ut_run_for(0.05);
    CHECK(svr->timer_interval == 0.1);
    ut_run_for(0.5);
    CHECK_EQ(is_closed(quick_one), 1);                  /* it timed out and is gone */
    CHECK_EQ(svr->clt_count, 1);
    ut_run_for(0.3);
    CHECK(svr->timer_interval == 1.0);                  /* back to once a second */
    /* a deadline does the same, and goes back when it is cancelled */
    CHECK_EQ(write(slow_one, "deadline\n", 9), 9);       /* 0.3s */
    ut_run_for(0.05);
    CHECK(svr->timer_interval == 0.15);
    CHECK_EQ(write(slow_one, "cancel\n", 7), 7);
    ut_run_for(0.4);
    CHECK(svr->timer_interval == 1.0);
    CHECK_EQ(svr->clt_count, 1);
    /* the last timeout is taken away: the timer stops */
    CHECK_EQ(write(slow_one, "slow\n", 5), 5);
    ut_run_for(1.3);
    CHECK_EQ(svr->clt_count, 1);
    CHECK(!nw_timer_active(&svr->timer));
    /* and starts again for the next one */
    CHECK_EQ(write(slow_one, "quick\n", 6), 6);
    ut_run_for(0.05);
    CHECK(nw_timer_active(&svr->timer));
    ut_run_for(0.5);
    CHECK_EQ(is_closed(slow_one), 1);
    close(slow_one);
    close(quick_one);
    nw_svr_release(svr);

    free(buf);
    free(big);
    return 0;
}
