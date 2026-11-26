/* wrapper/ut_rpc: request/reply round trip plus heartbeat keep-alive */
# include "common.h"
# include "ut_rpc_svr.h"
# include "ut_rpc_clt.h"

static int svr_recv;
static int clt_recv;
static int connected;
static rpc_clt *clt;

static void svr_on_recv_pkg(nw_ses *ses, rpc_pkg *pkg)
{
    svr_recv++;
    CHECK_EQ(pkg->command, 100);
    CHECK_EQ(pkg->pkg_type, RPC_PKG_TYPE_REQUEST);
    CHECK_EQ(pkg->sequence, 7);
    CHECK_EQ(pkg->req_id, 0x1122334455667788ULL);
    CHECK_EQ(pkg->ext_size, 3);
    CHECK(memcmp(pkg->ext, "ext", 3) == 0);
    CHECK_EQ(pkg->body_size, 5);
    CHECK(memcmp(pkg->body, "hello", 5) == 0);

    rpc_pkg reply;
    memcpy(&reply, pkg, sizeof(reply));
    reply.pkg_type = RPC_PKG_TYPE_REPLY;
    reply.result = 42;
    reply.ext_size = 0;
    reply.body = "world!";
    reply.body_size = 6;
    CHECK_EQ(rpc_send(ses, &reply), 0);
}

static void clt_on_recv_pkg(nw_ses *ses, rpc_pkg *pkg)
{
    clt_recv++;
    CHECK_EQ(pkg->command, 100);
    CHECK_EQ(pkg->pkg_type, RPC_PKG_TYPE_REPLY);
    CHECK_EQ(pkg->result, 42);
    CHECK_EQ(pkg->sequence, 7);
    CHECK_EQ(pkg->body_size, 6);
    CHECK(memcmp(pkg->body, "world!", 6) == 0);
}

static void clt_on_connect(nw_ses *ses, bool result)
{
    CHECK(result);
    connected++;

    rpc_pkg pkg;
    memset(&pkg, 0, sizeof(pkg));
    pkg.command = 100;
    pkg.pkg_type = RPC_PKG_TYPE_REQUEST;
    pkg.sequence = 7;
    pkg.req_id = 0x1122334455667788ULL;
    pkg.ext = "ext";
    pkg.ext_size = 3;
    pkg.body = "hello";
    pkg.body_size = 5;
    CHECK_EQ(rpc_clt_send(clt, &pkg), 0);
}

int main(void)
{
    nw_svr_bind bind;
    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1:0", &bind.addr, &bind.sock_type), 0);
    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.bind_arr = &bind;

    rpc_svr_type st;
    memset(&st, 0, sizeof(st));
    CHECK(rpc_svr_create(&cfg, &st) == NULL);
    st.on_recv_pkg = svr_on_recv_pkg;
    rpc_svr *svr = rpc_svr_create(&cfg, &st);
    CHECK(svr != NULL);
    CHECK_EQ(rpc_svr_start(svr), 0);

    char addr_str[64];
    snprintf(addr_str, sizeof(addr_str), "tcp@127.0.0.1:%d", ut_svr_port(svr->raw_svr, 0));
    rpc_clt_cfg ccfg;
    memset(&ccfg, 0, sizeof(ccfg));
    nw_addr addr;
    CHECK_EQ(nw_sock_cfg_parse(addr_str, &addr, &ccfg.sock_type), 0);
    ccfg.name = "test";
    ccfg.addr_count = 1;
    ccfg.addr_arr = &addr;

    rpc_clt_type ct;
    memset(&ct, 0, sizeof(ct));
    ct.on_recv_pkg = clt_on_recv_pkg;
    ct.on_connect = clt_on_connect;
    clt = rpc_clt_create(&ccfg, &ct);
    CHECK(clt != NULL);
    CHECK_EQ(rpc_clt_start(clt), 0);

    /* a connection that sends no heartbeat, next to the real client */
    int mute = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(mute >= 0);
    CHECK_EQ(connect(mute, NW_SOCKADDR(&addr), sizeof(addr.in)), 0);

    /* longer than RPC_HEARTBEAT_TIMEOUT: heartbeats must keep the link up */
    ut_run_for(RPC_HEARTBEAT_TIMEOUT + 1.5);
    CHECK_EQ(connected, 1);
    CHECK_EQ(svr_recv, 1);
    CHECK_EQ(clt_recv, 1);
    CHECK(rpc_clt_connected(clt));
    /* the mute one was dropped after RPC_HEARTBEAT_TIMEOUT, the client that
     * sends heartbeats is still there */
    char c;
    CHECK_EQ(recv(mute, &c, 1, MSG_DONTWAIT), 0);
    close(mute);
    CHECK_EQ(svr->raw_svr->clt_count, 1);

    rpc_clt_release(clt);
    ut_run_for(0.05);
    CHECK_EQ(svr->raw_svr->clt_count, 0);
    rpc_svr_release(svr);
    return 0;
}
