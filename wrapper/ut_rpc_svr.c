/*
 * Description: ut_rpc server, a nw_svr speaking rpc_pkg
 *     History: yang@haipo.me, 2016/03/29, create
 */

# include <stdlib.h>
# include <string.h>
# include <assert.h>

# include "ut_rpc_svr.h"
# include "ut_crc32.h"
# include "nw_sock.h"

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    struct rpc_pkg pkg;
    memcpy(&pkg, data, RPC_PKG_HEAD_SIZE);
    pkg.ext = data + RPC_PKG_HEAD_SIZE;
    pkg.body = pkg.ext + pkg.ext_size;

    if (pkg.command == RPC_CMD_HEARTBEAT) {
        /* send it back. that it arrived is what keeps the connection alive:
         * the idle timeout of the nw_svr underneath counts silence */
        if (ses->ses_type != NW_SES_TYPE_COMMON)
            return;
        rpc_send(ses, &pkg);
        return;
    }

    rpc_svr *svr = ((nw_svr *)ses->svr)->privdata;
    svr->on_recv_pkg(ses, &pkg);
}

static void on_open(nw_ses *ses)
{
    rpc_svr *svr = ((nw_svr *)ses->svr)->privdata;
    if (svr->on_open)
        svr->on_open(ses);
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
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
    raw_type.on_close = type->on_close;
    raw_type.on_open = on_open;
    raw_type.on_error_msg = on_error_msg;
    raw_type.on_recv_pkg = on_recv_pkg;

    cfg->max_pkg_size = RPC_MAX_PKG_SIZE;
    /* a client sends a heartbeat every RPC_HEARTBEAT_INTERVAL: one that is
     * silent for RPC_HEARTBEAT_TIMEOUT is gone */
    cfg->idle_timeout = RPC_HEARTBEAT_TIMEOUT;
    svr->raw_svr = nw_svr_create(cfg, &raw_type, svr);
    if (svr->raw_svr == NULL) {
        free(svr);
        return NULL;
    }

    svr->on_recv_pkg = type->on_recv_pkg;
    svr->on_open = type->on_open;

    return svr;
}

int rpc_svr_start(rpc_svr *svr)
{
    return nw_svr_start(svr->raw_svr);
}

int rpc_svr_stop(rpc_svr *svr)
{
    return nw_svr_stop(svr->raw_svr);
}

void rpc_svr_release(rpc_svr *svr)
{
    nw_svr_release(svr->raw_svr);
    free(svr);
}

void rpc_svr_close(rpc_svr *svr, nw_ses *ses)
{
    nw_svr_close(svr->raw_svr, ses);
}
