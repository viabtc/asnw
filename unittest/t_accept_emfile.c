/*
 * Regression (fixed): the accept loop used to spin when the process was out
 * of fds. on_can_accept() reported EMFILE and returned, the pending
 * connection stayed in the backlog, the listening fd stayed readable and
 * libev called back immediately, forever (about 170000 errors in 0.2s).
 * Now the connection is shed through a spare fd: one error per rejected
 * connection, the peer sees a close, and the server keeps working once fds
 * are available again.
 */
# include "common.h"
# include <sys/resource.h>

static int errors;

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    errors++;
    fprintf(stderr, "error: %s\n", msg);
}

int main(void)
{
    if (getenv("ASNW_UT_VALGRIND")) {
        /* valgrind manages RLIMIT_NOFILE itself, the fd shortage can't be staged */
        fprintf(stderr, "skipped under valgrind\n");
        return 0;
    }
    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = ut_line_decode;
    st.on_recv_pkg = on_recv_pkg;
    st.on_error_msg = on_error_msg;
    nw_svr *svr = ut_svr("tcp@127.0.0.1:0", &st, 1024, NULL);

    nw_addr addr;
    CHECK_EQ(nw_sock_host_addr(svr->bind_list[0].sockfd, &addr), 0);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    ut_run_for(0.01);                   /* let libev set up its own fds first */

    /* no fd left for accept() */
    struct rlimit rlim, saved;
    CHECK_EQ(getrlimit(RLIMIT_NOFILE, &rlim), 0);
    saved = rlim;
    rlim.rlim_cur = fd + 1;
    CHECK_EQ(setrlimit(RLIMIT_NOFILE, &rlim), 0);

    CHECK_EQ(connect(fd, NW_SOCKADDR(&addr), sizeof(addr.in)), 0);
    ut_run_for(0.2);
    CHECK_EQ(setrlimit(RLIMIT_NOFILE, &saved), 0);
    fprintf(stderr, "one pending connection produced %d accept errors in 0.2s\n", errors);
    CHECK_EQ(errors, 1);
    CHECK_EQ(svr->clt_count, 0);

    /* the rejected client was closed, not left hanging */
    char c;
    CHECK_EQ(recv(fd, &c, 1, MSG_DONTWAIT), 0);
    close(fd);

    /* with fds available again the server accepts as usual */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    CHECK_EQ(connect(fd, NW_SOCKADDR(&addr), sizeof(addr.in)), 0);
    ut_run_for(0.1);
    CHECK_EQ(errors, 1);
    CHECK_EQ(svr->clt_count, 1);
    close(fd);
    ut_run_for(0.1);
    CHECK_EQ(svr->clt_count, 0);
    nw_svr_release(svr);
    return 0;
}
