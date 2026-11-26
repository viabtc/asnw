/*
 * Regression (fixed): nw_clt_release() used to leave clt->timer running, so
 * libev later called back into freed memory. Release a client right after
 * it connected, while it waits to reconnect, and before it was ever started.
 */
# include "common.h"

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
}

static void on_connect(nw_ses *ses, bool result)
{
    CHECK(result);
    nw_loop_break();
}

int main(void)
{
    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = ut_line_decode;
    st.on_recv_pkg = on_recv_pkg;
    nw_svr *svr = ut_svr("tcp@127.0.0.1:0", &st, 1024, NULL);

    char cfg[64];
    snprintf(cfg, sizeof(cfg), "tcp@127.0.0.1:%d", ut_svr_port(svr, 0));
    nw_clt_type ct;
    memset(&ct, 0, sizeof(ct));
    ct.decode_pkg = ut_line_decode;
    ct.on_recv_pkg = on_recv_pkg;
    ct.on_connect = on_connect;
    nw_clt *clt = ut_clt(cfg, &ct, 1024, 0.2, NULL);
    CHECK_EQ(nw_clt_start(clt), 0);
    ut_run_for(1.0);
    CHECK(nw_clt_connected(clt));
    CHECK(!nw_timer_active(&clt->timer));   /* connect timeout disarmed on success */
    nw_clt_release(clt);
    ut_run_for(0.5);        /* a stale timer would fire in here */

    /* waiting for the next reconnect attempt */
    snprintf(cfg, sizeof(cfg), "tcp@127.0.0.1:%d", ut_free_port(SOCK_STREAM));
    ct.on_connect = NULL;
    clt = ut_clt(cfg, &ct, 1024, 0.2, NULL);
    CHECK_EQ(nw_clt_start(clt), 0);
    ut_run_for(0.1);
    CHECK(!nw_clt_connected(clt));
    CHECK(nw_timer_active(&clt->timer));
    nw_clt_release(clt);
    ut_run_for(0.5);

    /* never started */
    clt = ut_clt(cfg, &ct, 1024, 0.2, NULL);
    nw_clt_release(clt);

    /* started twice: restarts, the first socket is not leaked */
    snprintf(cfg, sizeof(cfg), "tcp@127.0.0.1:%d", ut_svr_port(svr, 0));
    ct.on_connect = on_connect;
    clt = ut_clt(cfg, &ct, 1024, 0.2, NULL);
    CHECK_EQ(nw_clt_start(clt), 0);
    CHECK_EQ(nw_clt_start(clt), 0);
    ut_run_for(1.0);
    CHECK(nw_clt_connected(clt));
    ut_run_for(0.05);
    CHECK_EQ(svr->ses_count, 1);
    nw_clt_release(clt);
    ut_run_for(0.05);
    CHECK_EQ(svr->ses_count, 0);
    nw_svr_release(svr);
    return 0;
}
