/*
 * nw_svr + nw_clt echo round trip over every supported transport:
 * tcp, udp, unix stream, unix dgram, unix seqpacket.
 * The stream cases push ~2MB through the connection to exercise the
 * pending write list on both sides.
 */
# include "common.h"

# define MAX_PKG 4096

struct ctx {
    const char *name;
    int total;
    int sent;
    int recv;
    int connect_ok;
    int svr_new;
    int svr_recv;
    int errors;
    nw_clt *clt;
};

static struct ctx *cur;

static int make_msg(char *buf, size_t size, int seq, int pad)
{
    int n = snprintf(buf, size, "%08d ", seq);
    for (int i = 0; i < pad; ++i)
        buf[n++] = 'a' + (seq + i) % 26;
    buf[n++] = '\n';
    return n;
}

static void svr_on_open(nw_ses *ses)
{
    cur->svr_new++;
}

static void svr_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    cur->svr_recv++;
    CHECK_EQ(nw_ses_send(ses, data, size), 0);
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    fprintf(stderr, "[%s] error: %s\n", cur->name, msg);
    cur->errors++;
}

static int pad_of(nw_ses *ses)
{
    return ses->sock_type == SOCK_STREAM ? 100 : 20;
}

static void clt_send(struct ctx *c, int count)
{
    char buf[MAX_PKG];
    int pad = pad_of(&c->clt->ses);
    for (int i = 0; i < count && c->sent < c->total; ++i, c->sent++) {
        int n = make_msg(buf, sizeof(buf), c->sent, pad);
        CHECK_EQ(nw_ses_send(&c->clt->ses, buf, n), 0);
    }
}

static void clt_on_connect(nw_ses *ses, bool result)
{
    CHECK(result);
    cur->connect_ok++;
    clt_send(cur, cur->total);
}

static void clt_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    char expect[MAX_PKG];
    int n = make_msg(expect, sizeof(expect), cur->recv, pad_of(ses));
    CHECK_EQ(size, n);
    CHECK(memcmp(data, expect, n) == 0);
    if (++cur->recv == cur->total)
        nw_loop_break();
    /* datagram sends are never queued by nw_ses (EAGAIN is an error),
     * so keep a small window in flight instead of bursting */
    if (ses->sock_type == SOCK_DGRAM)
        clt_send(cur, 1);
}

static void run_case(const char *name, const char *cfg_str, int total)
{
    struct ctx c;
    memset(&c, 0, sizeof(c));
    c.name = name;
    c.total = total;
    cur = &c;

    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = ut_line_decode;
    st.on_open = svr_on_open;
    st.on_recv_pkg = svr_on_recv_pkg;
    st.on_error_msg = on_error_msg;
    nw_svr *svr = ut_svr(strncmp(cfg_str, "tcp6@", 5) == 0 ? "tcp@[::1]:0" : cfg_str, &st, MAX_PKG, NULL);

    char real_cfg[256];
    snprintf(real_cfg, sizeof(real_cfg), "%s", cfg_str);
    if (strncmp(cfg_str, "tcp@", 4) == 0) {
        snprintf(real_cfg, sizeof(real_cfg), "tcp@127.0.0.1:%d", ut_svr_port(svr, 0));
    } else if (strncmp(cfg_str, "tcp6@", 5) == 0) {
        snprintf(real_cfg, sizeof(real_cfg), "tcp@[::1]:%d", ut_svr_port(svr, 0));
    } else if (strncmp(cfg_str, "udp@", 4) == 0) {
        snprintf(real_cfg, sizeof(real_cfg), "udp@127.0.0.1:%d", ut_svr_port(svr, 0));
    }

    nw_clt_type ct;
    memset(&ct, 0, sizeof(ct));
    ct.decode_pkg = ut_line_decode;
    ct.on_connect = clt_on_connect;
    ct.on_recv_pkg = clt_on_recv_pkg;
    ct.on_error_msg = on_error_msg;
    c.clt = ut_clt(real_cfg, &ct, MAX_PKG, 1.0, NULL);
    CHECK_EQ(nw_clt_start(c.clt), 0);

    bool stream = c.clt->ses.sock_type == SOCK_STREAM;
    bool dgram = c.clt->ses.sock_type == SOCK_DGRAM;
    if (dgram) {
        /* datagram clients are usable right after start, on_connect is not called */
        CHECK(nw_clt_connected(c.clt));
        clt_send(&c, 5);
    }

    ut_run_for(10.0);

    fprintf(stderr, "[%s] sent %d, server got %d, echoed back %d\n", name, c.sent, c.svr_recv, c.recv);
    CHECK_EQ(c.errors, 0);
    CHECK_EQ(c.sent, total);
    CHECK_EQ(c.svr_recv, total);
    CHECK_EQ(c.recv, total);
    CHECK_EQ(c.connect_ok, dgram ? 0 : 1);
    CHECK_EQ(c.svr_new, dgram ? 0 : 1);
    CHECK_EQ(svr->ses_count, dgram ? 0 : 1);
    if (stream)
        CHECK_EQ(c.clt->ses.write_buf->count, 0);

    /* a unix dgram client binds a private socket file and removes it on close */
    char local_path[sizeof(c.clt->ses.local_addr->un.sun_path)] = "";
    if (dgram && c.clt->ses.peer_addr.family == AF_UNIX) {
        strcpy(local_path, c.clt->ses.local_addr->un.sun_path);
        CHECK_EQ(access(local_path, F_OK), 0);
    }

    nw_clt_release(c.clt);
    ut_run_for(0.05);
    CHECK_EQ(svr->ses_count, 0);
    nw_svr_release(svr);
    if (local_path[0])
        CHECK_EQ(access(local_path, F_OK), -1);
}

int main(void)
{
    char cfg[256];

    run_case("tcp", "tcp@127.0.0.1:0", 20000);
    run_case("udp", "udp@127.0.0.1:0", 50);

    int v6 = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 in6;
    memset(&in6, 0, sizeof(in6));
    in6.sin6_family = AF_INET6;
    in6.sin6_addr = in6addr_loopback;
    if (v6 >= 0 && bind(v6, (struct sockaddr *)&in6, sizeof(in6)) == 0) {
        close(v6);
        run_case("tcp ipv6", "tcp6@", 2000);
    } else {
        fprintf(stderr, "[tcp ipv6] skipped: no ipv6 loopback on this host\n");
    }

    ut_unix_cfg(cfg, sizeof(cfg), "stream", "echo_stream.sock");
    run_case("unix stream", cfg, 20000);
    ut_unix_cfg(cfg, sizeof(cfg), "dgram", "echo_dgram.sock");
    run_case("unix dgram", cfg, 50);
    ut_unix_cfg(cfg, sizeof(cfg), "seqpacket", "echo_seqpacket.sock");
    run_case("unix seqpacket", cfg, 50);

    return 0;
}
