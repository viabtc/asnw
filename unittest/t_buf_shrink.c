/*
 * A nw_svr gives the bufs its pool has no use for back to the system,
 * from a timer of its own, once a minute. Here that timer is made to
 * fire every millisecond while a hundred connections send whole and
 * partial messages, are closed in the middle of one, and while a client
 * that does not read lets thousands of bufs pile up to be sent. Nothing
 * is lost, and the pool ends up small again. The point of running it is
 * the sanitizers: a buf freed while something still uses it would show
 * there.
 */
# include <fcntl.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <pthread.h>
# include "common.h"
# include "nw_hub.h"

# define CLIENTS 100
# define PKG 4096

static nw_svr *svr;
static int port;
static uint32_t freed, shrinks;
static size_t echoed;

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    /* -1 when the client has left: the session is closed by then */
    if (nw_ses_send(ses, data, size) == 0)
        echoed += size;
}

static int errors;

static void on_error_msg(nw_ses *ses, const char *msg)
{
    /* clients that leave while their echo is on its way */
    errors++;
}

/* what the timer of the server does when its minute is over. return the
 * number of bufs it gave back */
static uint32_t minute_over(nw_svr *s)
{
    nw_buf_pool *pool = s->buf_pool;
    uint32_t before = pool->free;
    s->shrink_timer.callback(&s->shrink_timer, s->shrink_timer.privdata);
    /* as many free bufs stay as there are in use, and not less than 64 */
    CHECK(pool->free <= (pool->used > NW_BUF_POOL_MIN_FREE ? pool->used : NW_BUF_POOL_MIN_FREE));
    CHECK(pool->free == before || pool->free >= NW_BUF_POOL_MIN_FREE);
    return before - pool->free;
}

static void on_fast_timer(nw_timer *timer, void *privdata)
{
    freed += minute_over(svr);
    shrinks++;
}

/* the servers of a hub are never started: they have the timer too */
static int with_timer;

static void on_worker(nw_hub *hub, void *data, size_t size)
{
    nw_svr *mine = nw_hub_svr(hub);
    if (mine && nw_timer_active(&mine->shrink_timer) && mine->shrink_timer.interval == 60.0)
        __sync_fetch_and_add(&with_timer, 1);
}

static int knock(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    in.sin_port = htons(port);
    CHECK_EQ(connect(fd, (struct sockaddr *)&in, sizeof(in)), 0);
    CHECK_EQ(fcntl(fd, F_SETFL, O_NONBLOCK), 0);
    return fd;
}

/* send it all, letting the server run when the socket is full */
static void push(int fd, const char *data, size_t size)
{
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += n;
        } else {
            CHECK(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
            ut_run_for(0.001);
        }
    }
}

/* read what has arrived, return how much */
static size_t drain(int fd)
{
    char buf[65536];
    size_t total = 0;
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        total += n;
    }
    return total;
}

static unsigned seed = 12345;

static unsigned next_random(void)
{
    seed = seed * 1103515245 + 12345;
    return seed >> 16;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = ut_line_decode;
    st.on_recv_pkg = on_recv_pkg;
    st.on_error_msg = on_error_msg;

    /* a server has the timer from the moment it is created, set to a
     * minute. the loop does not run for its sake: with nothing else to
     * do, nw_loop_run returns (it would hang here otherwise) */
    static nw_svr_bind bind;
    CHECK_EQ(nw_sock_addr_parse("tcp@127.0.0.1:0", &bind.addr, &bind.sock_type), 0);
    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.binds = &bind;
    cfg.max_pkg_size = PKG;
    nw_svr *idle = nw_svr_create(&cfg, &st, NULL);
    CHECK(idle != NULL);
    CHECK(nw_timer_active(&idle->shrink_timer));
    CHECK_EQ(idle->shrink_timer.interval, 60.0);
    double begin = ev_time();
    nw_loop_run();
    CHECK(ev_time() - begin < 1.0);
    CHECK_EQ(minute_over(idle), 0);                     /* nothing to give back */
    nw_svr_release(idle);
    nw_loop_run();                                      /* and the loop is as it was */

    /* so do the servers of the threads of a hub */
    nw_hub_cfg hc;
    memset(&hc, 0, sizeof(hc));
    hc.svr = cfg;
    hc.thread_count = 3;
    nw_hub_type ht;
    memset(&ht, 0, sizeof(ht));
    ht.svr = st;
    nw_hub *hub = nw_hub_create(&hc, &ht, NULL);
    CHECK(hub != NULL);
    CHECK_EQ(nw_hub_run_all(hub, on_worker, NULL, 0), 0);
    for (int i = 0; i < 400 && __sync_fetch_and_add(&with_timer, 0) < 3; ++i)
        usleep(5000);
    CHECK_EQ(__sync_fetch_and_add(&with_timer, 0), 3);
    nw_hub_release(hub);

    svr = ut_svr("tcp@127.0.0.1:0", &st, PKG, NULL);
    port = ut_svr_port(svr, 0);
    nw_buf_pool *pool = svr->buf_pool;

    static char line[PKG];
    memset(line, 'x', sizeof(line));
    line[PKG - 1] = '\n';

    int fds[CLIENTS];
    size_t sent[CLIENTS], received[CLIENTS];
    bool open_line[CLIENTS];
    size_t open_size[CLIENTS];
    for (int i = 0; i < CLIENTS; ++i) {
        fds[i] = knock();
        sent[i] = received[i] = 0;
        open_line[i] = false;
        open_size[i] = 0;
    }
    for (int i = 0; i < 400 && svr->ses_count < CLIENTS; ++i)
        ut_run_for(0.005);
    CHECK_EQ(svr->ses_count, CLIENTS);

    /* by hand first. half a message on every connection: a buf each */
    for (int i = 0; i < CLIENTS; ++i)
        push(fds[i], "half a message", 14);
    for (int i = 0; i < 400 && pool->used < CLIENTS; ++i)
        ut_run_for(0.005);
    CHECK_EQ(pool->used, CLIENTS);
    CHECK_EQ(minute_over(svr), 0);                      /* all of them are in use */
    for (int i = 0; i < CLIENTS; ++i)
        push(fds[i], "\n", 1);
    for (int i = 0; i < 400 && pool->used > 0; ++i)
        ut_run_for(0.005);
    CHECK_EQ(pool->used, 0);
    CHECK_EQ(pool->free, CLIENTS);
    CHECK_EQ(minute_over(svr), CLIENTS - NW_BUF_POOL_MIN_FREE);
    CHECK_EQ(pool->free, NW_BUF_POOL_MIN_FREE);
    for (int i = 0; i < CLIENTS; ++i)
        CHECK_EQ(drain(fds[i]), 15);
    echoed = 0;

    /* from now on the minute is over every millisecond, whatever goes on */
    nw_timer timer;
    nw_timer_set(&timer, 0.001, true, on_fast_timer, NULL);
    nw_timer_start(&timer);

    size_t lost_with_connection = 0;
    for (int round = 0; round < 150; ++round) {
        for (int i = 0; i < CLIENTS; ++i) {
            unsigned r = next_random();
            size_t n = r % 1000 + 1;
            if (r % 4 == 0 && open_size[i] + n < PKG - 1024) {
                /* a piece of a message, to be finished later */
                push(fds[i], line, n);
                sent[i] += n;
                open_line[i] = true;
                open_size[i] += n;
            } else if (r % 4 <= 1 || open_line[i]) {
                push(fds[i], line + PKG - n, n);        /* ends with the newline */
                sent[i] += n;
                open_line[i] = false;
                open_size[i] = 0;
            }
            received[i] += drain(fds[i]);
        }
        ut_run_for(0.002);
        /* some leave, in the middle of a message or not, and come back */
        if (round % 10 == 9) {
            for (int k = 0; k < 10; ++k) {
                int i = next_random() % CLIENTS;
                ut_run_for(0.002);
                received[i] += drain(fds[i]);
                lost_with_connection += sent[i] - received[i];
                close(fds[i]);
                fds[i] = knock();
                sent[i] = received[i] = 0;
                open_line[i] = false;
                open_size[i] = 0;
            }
        }
    }

    /* one of them sends a lot and reads nothing: its echoes wait in bufs */
    for (int i = 0; i < CLIENTS; ++i) {
        if (open_line[i]) {
            push(fds[i], "\n", 1);
            sent[i] += 1;
            open_line[i] = false;
        }
    }
    uint32_t peak = 0;
    for (int n = 0; n < 3000; ++n) {
        push(fds[0], line, PKG);
        sent[0] += PKG;
        if (pool->used > peak)
            peak = pool->used;
    }
    ut_run_for(0.05);
    if (pool->used > peak)
        peak = pool->used;
    fprintf(stderr, "bufs waiting to be sent at the most: %u\n", peak);
    CHECK(peak > 500);

    /* everything comes back in the end */
    for (int n = 0; n < 2000; ++n) {
        bool done = true;
        for (int i = 0; i < CLIENTS; ++i) {
            received[i] += drain(fds[i]);
            if (received[i] < sent[i])
                done = false;
        }
        if (done)
            break;
        ut_run_for(0.005);
    }
    for (int i = 0; i < CLIENTS; ++i)
        CHECK_EQ(received[i], sent[i]);

    /* and the pool is small again, without having been asked by hand */
    ut_run_for(0.02);
    CHECK_EQ(pool->used, 0);
    CHECK(pool->free <= NW_BUF_POOL_MIN_FREE);
    fprintf(stderr, "shrunk %u times, %u bufs given back, %zu bytes echoed, %zu lost with closed connections\n",
            shrinks, freed, echoed, lost_with_connection);
    CHECK(freed > peak / 2);
    CHECK(shrinks > 100);

    nw_timer_stop(&timer);
    for (int i = 0; i < CLIENTS; ++i)
        close(fds[i]);
    ut_run_for(0.05);
    CHECK_EQ(svr->ses_count, 0);
    nw_svr_release(svr);
    nw_loop_release();
    return 0;
}
