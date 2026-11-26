/*
 * Description: ut_rpc client, a nw_clt speaking rpc_pkg with failover
 *     History: yang@haipo.me, 2016/03/30, create
 */

# ifndef _UT_RPC_CLT_H_
# define _UT_RPC_CLT_H_

# include "ut_rpc.h"
# include "nw_clt.h"
# include "nw_timer.h"

/*
 * rpc_clt keeps a connection to one of several equivalent servers. it
 * starts with the first address; when a connection fails or is lost it
 * goes through the whole list without delay, from the first address on,
 * then waits reconnect_timeout before going through it again. heartbeats
 * are sent automatically (see ut_rpc.h).
 *
 * in the callbacks ses->privdata is the rpc_clt.
 */

typedef struct rpc_clt_cfg {
    /* must: a name for the peer, for your logs. copied */
    char *name;
    /* number of entries in addrs, at least 1 */
    uint32_t addr_count;
    /* the server addresses, all of the same sock_type. copied */
    nw_addr *addrs;
    /* SOCK_STREAM, SOCK_DGRAM or SOCK_SEQPACKET */
    int sock_type;
    /* see nw_clt_cfg */
    uint32_t read_mem;
    uint32_t write_mem;
    double reconnect_timeout;
} rpc_clt_cfg;

typedef struct rpc_clt_type {
    /* must
     *
     * called for every packet received, except heartbeats. the header is
     * in host byte order; pkg, pkg->ext and pkg->body are only valid
     * during the call */
    void (*on_recv_pkg)(nw_ses *ses, rpc_pkg *pkg);
    /* optional
     *
     * called when a connect attempt ends, see nw_clt_type.on_connect */
    void (*on_connect)(nw_ses *ses, bool result);
} rpc_clt_type;

/* the fields are for reading only */
typedef struct rpc_clt {
    char *name;
    nw_clt *raw_clt;
    uint32_t addr_count;
    uint32_t curr_index;
    nw_addr *addrs;
    nw_timer timer;
    double last_heartbeat;
    void (*on_recv_pkg)(nw_ses *ses, rpc_pkg *pkg);
    void (*on_connect)(nw_ses *ses, bool result);
} rpc_clt;

/* create a client. return NULL if name or on_recv_pkg is missing or
 * addr_count is 0 */
rpc_clt *rpc_clt_create(rpc_clt_cfg *cfg, rpc_clt_type *type);
/* begin to connect and start the heartbeat. return 0 or -1, see nw_clt_start */
int rpc_clt_start(rpc_clt *clt);
/* close the connection and stay closed until rpc_clt_start */
int rpc_clt_stop(rpc_clt *clt);
/* send a packet, see rpc_send. check rpc_clt_connected first */
int rpc_clt_send(rpc_clt *clt, rpc_pkg *pkg);
/* close and free the client. don't call it from one of its callbacks */
void rpc_clt_release(rpc_clt *clt);
/* is the connection established? */
bool rpc_clt_connected(rpc_clt *clt);

# endif
