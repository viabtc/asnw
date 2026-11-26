/*
 * Regression (fixed): sending to a peer that already closed raises SIGPIPE.
 * nw_ses uses write()/sendmsg() without MSG_NOSIGNAL and the library never
 * ignores SIGPIPE, so a client that disconnects with a reply still pending
 * kills the whole server process unless the application remembers to
 * signal(SIGPIPE, SIG_IGN) itself.
 */
# include "common.h"

static void on_sigpipe(int sig)
{
    static const char msg[] = "SIGPIPE raised by nw_ses_send\n";
    if (write(2, msg, sizeof(msg) - 1) < 0) {
    }
    _exit(1);
}

static int recv_count;

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    recv_count++;
    nw_ses_send(ses, data, size);
}

int main(void)
{
    signal(SIGPIPE, on_sigpipe);

    char cfg[256];
    ut_unix_cfg(cfg, sizeof(cfg), "stream", "sigpipe.sock");
    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = ut_line_decode;
    st.on_recv_pkg = on_recv_pkg;
    ut_svr(cfg, &st, 1024, NULL);

    /* a plain client: connect, send one request, hang up before the reply */
    nw_addr addr;
    int type;
    CHECK_EQ(nw_sock_addr_parse(cfg, &addr, &type), 0);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    CHECK_EQ(connect(fd, NW_SOCKADDR(&addr), addr.addrlen), 0);
    CHECK_EQ(write(fd, "ping\n", 5), 5);
    close(fd);

    ut_run_for(0.3);
    CHECK_EQ(recv_count, 1);
    return 0;
}
