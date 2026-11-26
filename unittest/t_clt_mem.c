/*
 * read_mem and write_mem of a nw_clt_cfg reach the socket of the client,
 * for a tcp client once it is connected and for a udp client at once;
 * left at 0 the socket keeps what the system gives it.
 */
# include "common.h"

/* the system doubles what is asked for, and reports the double */
# define READ_MEM  40000
# define WRITE_MEM 50000

static int connected;

static void on_connect(nw_ses *ses, bool result)
{
    CHECK(result);
    connected++;
}

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
}

static nw_clt *client(const char *proto, int port, bool set)
{
    char str[64];
    snprintf(str, sizeof(str), "%s@127.0.0.1:%d", proto, port);
    nw_clt_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK_EQ(nw_clt_cfg_parse(str, &cfg), 0);
    cfg.max_pkg_size = 1024;
    cfg.read_mem = set ? READ_MEM : 0;
    cfg.write_mem = set ? WRITE_MEM : 0;
    nw_clt_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = ut_line_decode;
    type.on_recv_pkg = on_recv_pkg;
    type.on_connect = on_connect;
    nw_clt *clt = nw_clt_create(&cfg, &type, NULL);
    CHECK(clt != NULL);
    CHECK_EQ(nw_clt_start(clt), 0);
    return clt;
}

static void check(nw_clt *clt, bool set)
{
    int read_mem = 0, write_mem = 0;
    CHECK(nw_clt_connected(clt));
    CHECK_EQ(nw_sock_get_recv_buf(clt->ses.sockfd, &read_mem), 0);
    CHECK_EQ(nw_sock_get_send_buf(clt->ses.sockfd, &write_mem), 0);
    if (set) {
        CHECK_EQ(read_mem, READ_MEM * 2);
        CHECK_EQ(write_mem, WRITE_MEM * 2);
    } else {
        CHECK(read_mem != READ_MEM * 2);
        CHECK(write_mem != WRITE_MEM * 2);
    }
    nw_clt_release(clt);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = ut_line_decode;
    st.on_recv_pkg = on_recv_pkg;
    nw_svr *tcp = ut_svr("tcp@127.0.0.1:0", &st, 1024, NULL);
    nw_svr *udp = ut_svr("udp@127.0.0.1:0", &st, 1024, NULL);

    for (int set = 0; set < 2; ++set) {
        connected = 0;
        nw_clt *clt = client("tcp", ut_svr_port(tcp, 0), set);
        for (int i = 0; i < 400 && !connected; ++i)
            ut_run_for(0.005);
        CHECK_EQ(connected, 1);
        check(clt, set);
        /* a udp client has its socket as soon as it is started */
        check(client("udp", ut_svr_port(udp, 0), set), set);
    }

    nw_svr_release(tcp);
    nw_svr_release(udp);
    nw_loop_release();
    return 0;
}
