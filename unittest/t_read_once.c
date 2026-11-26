/*
 * Reading a stream: one read per event when the data fits in the buffer
 * (a read that returns less than was asked for has emptied the socket, a
 * second one would only come back with EAGAIN), and reading on while the
 * buffer keeps coming back full.
 */
# include <sys/socket.h>
# include "common.h"

static int recv_count, close_count;
static size_t recv_bytes;

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    recv_count++;
    recv_bytes += size;
    /* fails for the client that has left by then, which closes the
     * connection a little earlier than its end of stream would */
    nw_ses_send(ses, "ok\n", 3);
}

static void on_connection_close(nw_ses *ses)
{
    close_count++;
}

/* read system calls made by this process so far, -1 if the kernel does
 * not say */
static long read_calls(void)
{
    FILE *f = fopen("/proc/self/io", "r");
    if (f == NULL)
        return -1;
    char line[128];
    long count = -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "syscr: %ld", &count) == 1)
            break;
    }
    fclose(f);
    return count;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = ut_line_decode;
    type.on_recv_pkg = on_recv_pkg;
    type.on_connection_close = on_connection_close;
    nw_svr *svr = ut_svr("tcp@127.0.0.1:0", &type, 1024, NULL);
    nw_addr addr;
    CHECK_EQ(nw_sock_host_addr(svr->bind_list[0].sockfd, &addr), 0);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK_EQ(connect(fd, NW_SOCKADDR(&addr), sizeof(addr.in)), 0);
    ut_run_for(0.05);

    /* 200 requests, one at a time: one read each */
    char reply[16];
    long before = read_calls();
    for (int i = 0; i < 200; ++i) {
        CHECK_EQ(send(fd, "ping\n", 5, 0), 5);
        ut_run_for(0.001);
        CHECK_EQ(recv(fd, reply, sizeof(reply), MSG_DONTWAIT), 3);
    }
    long after = read_calls();
    CHECK_EQ(recv_count, 200);
    if (getenv("ASNW_UT_VALGRIND")) {
        /* valgrind reads on its own account in the same process */
        fprintf(stderr, "skipped: no counting under valgrind\n");
    } else if (before >= 0 && after >= 0) {
        /* the fopen and fgets above read too, hence the margin */
        fprintf(stderr, "200 requests: %ld read calls\n", after - before);
        CHECK(after - before >= 200 && after - before < 240);
    } else {
        fprintf(stderr, "skipped: /proc/self/io not readable\n");
    }

    /* much more than one buffer in one go: the buffer comes back full,
     * so reading goes on until everything is in */
    static char big[40 * 500];
    for (int i = 0; i < 40; ++i) {
        memset(big + i * 500, 'a' + i % 26, 499);
        big[i * 500 + 499] = '\n';
    }
    recv_count = 0;
    recv_bytes = 0;
    CHECK_EQ(send(fd, big, sizeof(big), 0), (ssize_t)sizeof(big));
    ut_run_for(0.1);
    CHECK_EQ(recv_count, 40);
    CHECK_EQ(recv_bytes, sizeof(big));

    /* data that exactly fills the buffer and then nothing: no message is
     * lost, the connection stays */
    static char full[1024];
    memset(full, 'x', sizeof(full));
    full[511] = '\n';
    full[1023] = '\n';
    recv_count = 0;
    CHECK_EQ(send(fd, full, sizeof(full), 0), (ssize_t)sizeof(full));
    ut_run_for(0.05);
    CHECK_EQ(recv_count, 2);
    CHECK_EQ(svr->clt_count, 1);

    /* the last bytes and the close arrive together: both are seen */
    recv_count = 0;
    CHECK_EQ(send(fd, "last\n", 5, 0), 5);
    close(fd);
    ut_run_for(0.05);
    CHECK_EQ(recv_count, 1);
    CHECK_EQ(close_count, 1);
    CHECK_EQ(svr->clt_count, 0);

    /* a connection that is shutting down still notices the peer leave */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK_EQ(connect(fd, NW_SOCKADDR(&addr), sizeof(addr.in)), 0);
    ut_run_for(0.05);
    nw_svr_iterator iter;
    nw_svr_iter_init(&iter, svr);
    nw_ses *ses = nw_svr_next(&iter);
    CHECK(ses != NULL);
    nw_svr_shutdown_clt(svr, ses);
    CHECK_EQ(send(fd, "ignored\n", 8, 0), 8);
    ut_run_for(0.05);
    CHECK_EQ(svr->clt_count, 1);
    close(fd);
    ut_run_for(0.1);
    CHECK_EQ(svr->clt_count, 0);
    CHECK_EQ(close_count, 2);

    nw_svr_release(svr);
    return 0;
}
