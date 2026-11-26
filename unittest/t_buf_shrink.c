/*
 * nw_buf_pool_shrink on the pool of a server at work. A timer of the
 * program calls it every millisecond while a hundred connections send
 * whole and partial messages, are closed in the middle of one, and while
 * a client that does not read lets thousands of bufs pile up to be sent.
 * Nothing is lost, and the pool ends up small again. The point of running
 * it is the sanitizers: a buf freed while something still uses it would
 * show there.
 */
# include <fcntl.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include "common.h"

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

static void on_shrink_timer(nw_timer *timer, void *privdata)
{
    freed += nw_buf_pool_shrink(svr->buf_pool);
    shrinks++;
    /* what is kept is what the rule says */
    nw_buf_pool *pool = svr->buf_pool;
    CHECK(pool->free <= (pool->used > NW_BUF_POOL_MIN_FREE ? pool->used : NW_BUF_POOL_MIN_FREE));
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
    CHECK_EQ(nw_buf_pool_shrink(pool), 0);              /* all of them are in use */
    for (int i = 0; i < CLIENTS; ++i)
        push(fds[i], "\n", 1);
    for (int i = 0; i < 400 && pool->used > 0; ++i)
        ut_run_for(0.005);
    CHECK_EQ(pool->used, 0);
    CHECK_EQ(pool->free, CLIENTS);
    CHECK_EQ(nw_buf_pool_shrink(pool), CLIENTS - NW_BUF_POOL_MIN_FREE);
    CHECK_EQ(pool->free, NW_BUF_POOL_MIN_FREE);
    for (int i = 0; i < CLIENTS; ++i)
        CHECK_EQ(drain(fds[i]), 15);
    echoed = 0;

    /* from now on a timer does it, whatever goes on */
    nw_timer timer;
    nw_timer_set(&timer, 0.001, true, on_shrink_timer, NULL);
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
