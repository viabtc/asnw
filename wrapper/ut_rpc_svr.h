/*
 * Description: ut_rpc server, a nw_svr speaking rpc_pkg
 *     History: yang@haipo.me, 2016/03/29, create
 */

# ifndef _UT_RPC_SVR_H_
# define _UT_RPC_SVR_H_

# include "ut_rpc.h"
# include "nw_svr.h"

/*
 * rpc_svr is a nw_svr that receives and sends rpc_pkg. it answers the
 * heartbeats of its clients and closes connections that stay silent for
 * RPC_HEARTBEAT_TIMEOUT seconds (see ut_rpc.h), using the idle timeout of
 * nw_svr: anything a client sends counts, not only heartbeats.
 *
 * in the callbacks, reach the rpc_svr with ((nw_svr *)ses->svr)->privdata,
 * and reply with rpc_send(ses, &pkg). ses->privdata is not used by rpc_svr
 * and starts as NULL. the session lifetime rules of nw_svr.h apply.
 */

typedef struct rpc_svr_type {
    /* must
     *
     * called for every packet received, except heartbeats. the header is
     * in host byte order; pkg, pkg->ext and pkg->body are only valid
     * during the call */
    void (*on_recv_pkg)(nw_ses *ses, rpc_pkg *pkg);
    /* optional, see nw_svr_type.on_open */
    void (*on_open)(nw_ses *ses);
    /* optional, see nw_svr_type.on_close */
    void (*on_close)(nw_ses *ses);
} rpc_svr_type;

/* the fields are for reading only */
typedef struct rpc_svr {
    nw_svr *raw_svr;
    void (*on_recv_pkg)(nw_ses *ses, rpc_pkg *pkg);
    void (*on_open)(nw_ses *ses);
} rpc_svr;

/* create a server from a nw_svr_cfg. cfg->max_pkg_size is overwritten with
 * RPC_MAX_PKG_SIZE and cfg->idle_timeout with RPC_HEARTBEAT_TIMEOUT. return NULL if on_recv_pkg is missing or nw_svr_create
 * fails */
rpc_svr *rpc_svr_create(nw_svr_cfg *cfg, rpc_svr_type *type);
/* bind and start accepting. return 0 or -1, see nw_svr_start */
int rpc_svr_start(rpc_svr *svr);
/* stop accepting new connections, see nw_svr_stop */
int rpc_svr_stop(rpc_svr *svr);
/* close every connection and free the server */
void rpc_svr_release(rpc_svr *svr);
/* close one connection, see nw_svr_close */
void rpc_svr_close(rpc_svr *svr, nw_ses *ses);

# endif
