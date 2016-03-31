/*
 * Description: 
 *     History: yang@haipo.me, 2016/03/29, create
 */

# include <assert.h>

# include "ut_rpc_svr.h"
# include "ut_misc.h"
# include "ut_crc32.h"
# include "ut_log.h"
# include "nw_sock.h"

struct clt_info {
    double last_heartbeat;
};

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    struct rpc_pkg pkg;
    memcpy(&pkg, data, RPC_PKG_HEAD_SIZE);
    pkg.ext = data + RPC_PKG_HEAD_SIZE;
    pkg.body = pkg.ext + pkg.ext_size;

    if (pkg.command == RPC_CMD_HEARTBEAT) {
        if (ses->ses_type != NW_SES_TYPE_COMMON)
            return;
        struct clt_info *info = ses->privdata;
        info->last_heartbeat = current_timestamp();
        rpc_send(ses, &pkg);
        return;
    }

    rpc_svr *svr = ((nw_svr *)ses->svr)->privdata;
    svr->on_recv_pkg(ses, &pkg);
}

static void on_new_connection(nw_ses *ses)
{
    struct clt_info *info = ses->privdata;
    info->last_heartbeat = current_timestamp();
    rpc_svr *svr = ((nw_svr *)ses->svr)->privdata;
    if (svr->on_new_connection)
        svr->on_new_connection(ses);
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    log_error("peer: %s: %s", nw_sock_human_addr(&ses->peer_addr), msg);
}

static void *on_privdata_alloc(void *svr)
{
    rpc_svr *r_svr = ((nw_svr *)svr)->privdata;
    void *privdata = nw_cache_alloc(r_svr->privdata_cache);
    assert(privdata != NULL);
    return privdata;
}

static void on_privdata_free(void *svr, void *privdata)
{
    rpc_svr *r_svr = ((nw_svr *)svr)->privdata;
    return nw_cache_free(r_svr->privdata_cache, privdata);
}

static void on_timer(nw_timer *timer, void *privdata)
{
    rpc_svr *svr = privdata;
    double now = current_timestamp();

    nw_ses *curr = svr->raw_svr->clt_list_head;
    nw_ses *next;
    while (curr) {
        next = curr->next;
        struct clt_info *info = curr->privdata;
        if (now - info->last_heartbeat > RPC_HEARTBEAT_TIMEOUT) {
            log_error("peer: %s: heartbeat timeout", nw_sock_human_addr(&curr->peer_addr));
            nw_svr_close_clt(svr->raw_svr, curr);
        }
        curr = next;
    }
}

rpc_svr *rpc_svr_create(nw_svr_cfg *cfg, rpc_svr_type *type)
{
    if (type->on_recv_pkg == NULL)
        return NULL;

    rpc_svr *svr = malloc(sizeof(rpc_svr));
    assert(svr != NULL);

    nw_svr_type raw_type;
    memset(&raw_type, 0, sizeof(raw_type));
    raw_type.decode_pkg = rpc_decode;
    raw_type.on_connection_close = type->on_connection_close;
    raw_type.on_new_connection = on_new_connection;
    raw_type.on_error_msg = on_error_msg;
    raw_type.on_recv_pkg = on_recv_pkg;
    raw_type.on_privdata_alloc = on_privdata_alloc;
    raw_type.on_privdata_free = on_privdata_free;

    cfg->max_pkg_size = RPC_MAX_PKG_SIZE;
    svr->raw_svr = nw_svr_create(cfg, &raw_type, svr);
    if (svr->raw_svr == NULL) {
        free(svr);
        return NULL;
    }

    nw_timer_set(&svr->timer, RPC_HEARTBEAT_INTERVAL, true, on_timer, svr);
    svr->privdata_cache = nw_cache_create(sizeof(struct clt_info));
    assert(svr->privdata_cache != NULL);
    svr->on_recv_pkg = type->on_recv_pkg;
    svr->on_new_connection = type->on_new_connection;

    return svr;
}

int rpc_svr_start(rpc_svr *svr)
{
    int ret = nw_svr_start(svr->raw_svr);
    if (ret < 0)
        return ret;
    nw_timer_start(&svr->timer);
    return 0;
}

int rpc_svr_stop(rpc_svr *svr)
{
    int ret = nw_svr_stop(svr->raw_svr);
    if (ret < 0)
        return ret;
    nw_timer_stop(&svr->timer);
    return 0;
}

void rpc_svr_release(rpc_svr *svr)
{
    nw_svr_release(svr->raw_svr);
    nw_timer_stop(&svr->timer);
    nw_cache_release(svr->privdata_cache);
    free(svr);
}

void rpc_svr_close_clt(rpc_svr *svr, nw_ses *ses)
{
    nw_svr_close_clt(svr->raw_svr, ses);
}

