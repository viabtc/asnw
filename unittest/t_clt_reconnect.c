/*
 * nw_clt keeps retrying until the server shows up, and reconnects after
 * the server drops it. Uses tcp, where a refused connect is reported
 * asynchronously.
 */
# include "common.h"

static int connect_ok;
static int connect_fail;
static int close_count;
static int svr_new;
static nw_svr *svr;

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
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

static int clt_on_close(nw_ses *ses)
{
    close_count++;
    return 0;
}

static void svr_on_open(nw_ses *ses)
{
    svr_new++;
}

int main(void)
{
    char cfg[64];
    snprintf(cfg, sizeof(cfg), "tcp@127.0.0.1:%d", ut_free_port(SOCK_STREAM));

    nw_clt_type ct;
    memset(&ct, 0, sizeof(ct));
    ct.decode_pkg = ut_line_decode;
    ct.on_recv_pkg = on_recv_pkg;
    ct.on_connect = clt_on_connect;
    ct.on_close = clt_on_close;
    nw_clt *clt = ut_clt(cfg, &ct, 1024, 0.05, NULL);
    CHECK_EQ(nw_clt_start(clt), 0);

    /* nobody is listening yet */
    ut_run_for(0.3);
    CHECK_EQ(connect_ok, 0);
    CHECK(connect_fail >= 2);
    CHECK_EQ(close_count, connect_fail);
    CHECK(!nw_clt_connected(clt));

    /* server appears, the next retry gets through */
    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = ut_line_decode;
    st.on_recv_pkg = on_recv_pkg;
    st.on_open = svr_on_open;
    svr = ut_svr(cfg, &st, 1024, NULL);
    ut_run_for(2.0);
    CHECK_EQ(connect_ok, 1);
    CHECK(nw_clt_connected(clt));
    ut_run_for(0.05);
    CHECK_EQ(svr_new, 1);
    CHECK_EQ(svr->ses_count, 1);

    /* server drops the connection, client notices and comes back */
    int closes = close_count;
    nw_svr_iterator iter;
    nw_svr_iter_init(&iter, svr);
    nw_svr_close(svr, nw_svr_next(&iter));
    CHECK_EQ(svr->ses_count, 0);
    ut_run_for(2.0);
    CHECK_EQ(close_count, closes + 1);
    CHECK_EQ(connect_ok, 2);
    ut_run_for(0.05);
    CHECK_EQ(svr_new, 2);

    return 0;
}
