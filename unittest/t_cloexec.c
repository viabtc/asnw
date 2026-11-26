/*
 * Every socket the library creates or accepts is non-blocking and
 * close-on-exec: a program started by the application (system, popen,
 * fork + exec) does not inherit the listening sockets and connections.
 * Inherited, a connection would stay open after the application closed it
 * and the listening port would stay taken after it exited.
 */
# include <sys/socket.h>
# include <sys/wait.h>
# include "common.h"

static int connected;
static int received_fd = -1;

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
}

static void on_connect(nw_ses *ses, bool result)
{
    connected += result;
}

static void on_recv_fd(nw_ses *ses, int fd)
{
    received_fd = fd;
}

static void check_fd(int fd, bool nonblock)
{
    CHECK(fd >= 0);
    CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    if (nonblock)
        CHECK(fcntl(fd, F_GETFL) & O_NONBLOCK);
}

int main(void)
{
    char unix_cfg[128], packet_cfg[128], tcp_cfg[64];
    ut_unix_cfg(unix_cfg, sizeof(unix_cfg), "stream", "cloexec.sock");
    ut_unix_cfg(packet_cfg, sizeof(packet_cfg), "seqpacket", "cloexec-packet.sock");
    nw_svr_bind binds[3];
    CHECK_EQ(nw_sock_addr_parse("tcp@127.0.0.1:0", &binds[0].addr, &binds[0].sock_type), 0);
    CHECK_EQ(nw_sock_addr_parse(unix_cfg, &binds[1].addr, &binds[1].sock_type), 0);
    CHECK_EQ(nw_sock_addr_parse(packet_cfg, &binds[2].addr, &binds[2].sock_type), 0);
    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 3;
    cfg.binds = binds;
    cfg.max_pkg_size = 1024;
    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = ut_line_decode;
    st.on_recv_pkg = on_recv_pkg;
    st.on_recv_fd = on_recv_fd;
    nw_svr *svr = nw_svr_create(&cfg, &st, NULL);
    CHECK(svr != NULL);
    CHECK_EQ(nw_svr_start(svr), 0);
    snprintf(tcp_cfg, sizeof(tcp_cfg), "tcp@127.0.0.1:%d", ut_svr_port(svr, 0));

    nw_clt_type ct;
    memset(&ct, 0, sizeof(ct));
    ct.decode_pkg = ut_line_decode;
    ct.on_recv_pkg = on_recv_pkg;
    ct.on_connect = on_connect;
    nw_clt *clts[3];
    clts[0] = ut_clt(tcp_cfg, &ct, 1024, 1.0, NULL);
    clts[1] = ut_clt(unix_cfg, &ct, 1024, 1.0, NULL);
    clts[2] = ut_clt(packet_cfg, &ct, 1024, 1.0, NULL);
    for (int i = 0; i < 3; ++i)
        CHECK_EQ(nw_clt_start(clts[i]), 0);
    ut_run_for(0.1);
    CHECK_EQ(connected, 3);
    CHECK_EQ(svr->ses_count, 3);

    /* the flags of each kind of socket */
    for (int i = 0; i < 3; ++i) {
        check_fd(svr->binds[i].sockfd, true);        /* listening */
        check_fd(clts[i]->ses.sockfd, true);            /* connecting side */
    }
    nw_svr_iterator iter;
    nw_ses *ses;
    int accepted = 0;
    nw_svr_iter_init(&iter, svr);
    while ((ses = nw_svr_next(&iter)) != NULL) {
        check_fd(ses->sockfd, true);                    /* accepted */
        accepted++;
    }
    CHECK_EQ(accepted, 3);

    /* a file descriptor passed over a seqpacket connection */
    int pair[2];
    CHECK_EQ(pipe(pair), 0);
    CHECK(nw_ses_send_fd(&clts[2]->ses, pair[0]) > 0);
    ut_run_for(0.05);
    check_fd(received_fd, false);

    /* a connection added from outside is made so */
    int outside = socket(AF_INET, SOCK_STREAM, 0);
    nw_addr addr;
    CHECK_EQ(nw_sock_local_addr(svr->binds[0].sockfd, &addr), 0);
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(in);
    CHECK_EQ(bind(lfd, (struct sockaddr *)&in, len), 0);
    CHECK_EQ(listen(lfd, 4), 0);
    CHECK_EQ(getsockname(lfd, (struct sockaddr *)&in, &len), 0);
    CHECK_EQ(connect(outside, (struct sockaddr *)&in, sizeof(in)), 0);
    CHECK(!(fcntl(outside, F_GETFD) & FD_CLOEXEC));
    CHECK_EQ(nw_svr_add_fd(svr, outside), 0);
    check_fd(outside, true);

    /*
     * the proof: a program started from here sees none of them. it lists
     * what its own file descriptors point to; with the library's sockets
     * inherited there would be "socket:" entries. the three sockets made
     * by this test itself are closed first.
     */
    close(lfd);
    close(pair[0]);
    close(pair[1]);
    close(received_fd);
    CHECK_EQ(svr->ses_count, 4);
    FILE *child = popen("for f in /proc/self/fd/*; do echo \"${f##*/} $(readlink $f)\"; done 2>/dev/null", "r");
    CHECK(child != NULL);
    char line[256], target[200];
    int entries = 0, sockets = 0, number;
    while (fgets(line, sizeof(line), child)) {
        if (sscanf(line, "%d %199s", &number, target) != 2)
            continue;
        entries++;
        /* 0, 1 and 2 are whatever this test was started with */
        if (number > 2 && strncmp(target, "socket:", 7) == 0) {
            sockets++;
            fprintf(stderr, "inherited: %s", line);
        }
    }
    int status = pclose(child);
    CHECK(WIFEXITED(status));
    CHECK(entries >= 3);                /* it did list something */
    CHECK_EQ(sockets, 0);

    for (int i = 0; i < 3; ++i)
        nw_clt_release(clts[i]);
    nw_svr_release(svr);
    return 0;
}
