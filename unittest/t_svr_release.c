/*
 * Regression (fixed): nw_svr_release() used to free svr->clt_cache twice
 * (and leak the listeners' host_addr). Release an idle server, one with
 * live connections and pending data, and one bound to several addresses.
 */
# include "common.h"

static int alloc_count;
static int free_count;
static int close_count;

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    nw_ses_send(ses, data, size);
}

static void on_connection_close(nw_ses *ses)
{
    close_count++;
}

static void *on_privdata_alloc(void *svr)
{
    alloc_count++;
    return malloc(32);
}

static void on_privdata_free(void *svr, void *privdata)
{
    free_count++;
    free(privdata);
}

int main(void)
{
    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = ut_line_decode;
    type.on_recv_pkg = on_recv_pkg;
    type.on_connection_close = on_connection_close;
    type.on_privdata_alloc = on_privdata_alloc;
    type.on_privdata_free = on_privdata_free;

    /* never used */
    nw_svr *svr = ut_svr("tcp@127.0.0.1:0", &type, 1024, NULL);
    nw_svr_release(svr);

    /* three live connections, one of them with a half received message */
    svr = ut_svr("tcp@127.0.0.1:0", &type, 1024, NULL);
    nw_addr addr;
    CHECK_EQ(nw_sock_host_addr(svr->svr_list[0].sockfd, &addr), 0);
    int fds[3];
    for (int i = 0; i < 3; ++i) {
        fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(fds[i] >= 0);
        CHECK_EQ(connect(fds[i], NW_SOCKADDR(&addr), sizeof(addr.in)), 0);
    }
    CHECK_EQ(write(fds[0], "partial", 7), 7);
    CHECK_EQ(write(fds[1], "full\n", 5), 5);
    ut_run_for(0.1);
    CHECK_EQ(svr->clt_count, 3);
    CHECK_EQ(alloc_count, 3);
    nw_svr_release(svr);
    CHECK_EQ(close_count, 3);
    CHECK_EQ(free_count, 3);
    char buf[16];
    CHECK_EQ(read(fds[1], buf, sizeof(buf)), 5);    /* the echo */
    for (int i = 0; i < 3; ++i) {
        CHECK_EQ(read(fds[i], buf, sizeof(buf)), 0);    /* then closed by the server */
        close(fds[i]);
    }

    /* several binds of different kinds */
    char cfg[256];
    nw_svr_bind binds[3];
    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1:0", &binds[0].addr, &binds[0].sock_type), 0);
    CHECK_EQ(nw_sock_cfg_parse("udp@127.0.0.1:0", &binds[1].addr, &binds[1].sock_type), 0);
    ut_unix_cfg(cfg, sizeof(cfg), "seqpacket", "release.sock");
    CHECK_EQ(nw_sock_cfg_parse(cfg, &binds[2].addr, &binds[2].sock_type), 0);
    nw_svr_cfg svr_cfg;
    memset(&svr_cfg, 0, sizeof(svr_cfg));
    svr_cfg.bind_count = 3;
    svr_cfg.bind_arr = binds;
    svr_cfg.max_pkg_size = 1024;
    svr = nw_svr_create(&svr_cfg, &type, NULL);
    CHECK(svr != NULL);
    nw_svr_release(svr);                            /* created, never started */
    svr = nw_svr_create(&svr_cfg, &type, NULL);
    CHECK(svr != NULL);
    CHECK_EQ(nw_svr_start(svr), 0);
    nw_svr_release(svr);

    /* binding a unix address must not delete a regular file that is in the way */
    char path[128];
    snprintf(path, sizeof(path), "%s/not-a-socket", ut_tmpdir());
    int fd = open(path, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0);
    close(fd);
    ut_unix_cfg(cfg, sizeof(cfg), "stream", "not-a-socket");
    CHECK_EQ(nw_sock_cfg_parse(cfg, &binds[0].addr, &binds[0].sock_type), 0);
    svr_cfg.bind_count = 1;
    svr = nw_svr_create(&svr_cfg, &type, NULL);
    CHECK(svr != NULL);
    CHECK(nw_svr_start(svr) < 0);
    CHECK_EQ(access(path, F_OK), 0);
    nw_svr_release(svr);

    return 0;
}
