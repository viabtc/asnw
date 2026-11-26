/*
 * wrapper/ut_rpc_clt: with the first address dead, the client moves on to
 * the next one. Also a regression test: rpc_clt.curr_index used to be read
 * uninitialised when the very first connect failed (visible under valgrind).
 */
# include "common.h"
# include "ut_rpc_svr.h"
# include "ut_rpc_clt.h"
# include "ut_crc32.h"

static int connect_ok;
static int connect_fail;

static void on_recv_pkg(nw_ses *ses, rpc_pkg *pkg)
{
}

static void clt_on_connect(nw_ses *ses, bool result)
{
    if (result) {
        connect_ok++;
        nw_loop_break();
    } else {
        connect_fail++;
    }
}

int main(void)
{
    /* the checksum is the standard CRC-32 (check value of "123456789") */
    CHECK_EQ(generate_crc32("123456789", 9), 0xCBF43926u);

    nw_svr_bind bind;
    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1:0", &bind.addr, &bind.sock_type), 0);
    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.bind_arr = &bind;
    rpc_svr_type st;
    memset(&st, 0, sizeof(st));
    st.on_recv_pkg = on_recv_pkg;
    rpc_svr *svr = rpc_svr_create(&cfg, &st);
    CHECK(svr != NULL);
    CHECK_EQ(rpc_svr_start(svr), 0);

    char dead[64], alive[64];
    snprintf(dead, sizeof(dead), "tcp@127.0.0.1:%d", ut_free_port(SOCK_STREAM));
    snprintf(alive, sizeof(alive), "tcp@127.0.0.1:%d", ut_svr_port(svr->raw_svr, 0));
    nw_addr addrs[2];
    rpc_clt_cfg ccfg;
    memset(&ccfg, 0, sizeof(ccfg));
    CHECK_EQ(nw_sock_cfg_parse(dead, &addrs[0], &ccfg.sock_type), 0);
    CHECK_EQ(nw_sock_cfg_parse(alive, &addrs[1], &ccfg.sock_type), 0);
    ccfg.name = "failover";
    ccfg.addr_count = 2;
    ccfg.addr_arr = addrs;
    ccfg.reconnect_timeout = 30.0;      /* the second address must be tried without waiting */

    rpc_clt_type ct;
    memset(&ct, 0, sizeof(ct));
    ct.on_recv_pkg = on_recv_pkg;
    ct.on_connect = clt_on_connect;
    rpc_clt *clt = rpc_clt_create(&ccfg, &ct);
    CHECK(clt != NULL);
    CHECK_EQ(clt->curr_index, 0);
    CHECK_EQ(rpc_clt_start(clt), 0);

    ut_run_for(3.0);
    CHECK_EQ(connect_ok, 1);
    CHECK(connect_fail >= 1);
    CHECK(rpc_clt_connected(clt));
    CHECK(strcmp(nw_sock_human_addr(&clt->raw_clt->ses.peer_addr), alive + 4) == 0);
    ut_run_for(0.05);
    CHECK_EQ(svr->raw_svr->clt_count, 1);

    rpc_clt_release(clt);
    ut_run_for(0.05);
    rpc_svr_release(svr);
    return 0;
}
