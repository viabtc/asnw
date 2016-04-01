/*
 * Description: 
 *     History: yang@haipo.me, 2016/03/30, create
 */

# include <assert.h>

# include "ut_rpc_clt.h"
# include "ut_crc32.h"
# include "nw_sock.h"

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    struct rpc_pkg pkg;
    memcpy(&pkg, data, RPC_PKG_HEAD_SIZE);
    pkg.ext = data + RPC_PKG_HEAD_SIZE;
    pkg.body = pkg.ext + pkg.ext_size;

    rpc_clt *clt= ses->privdata;
    if (pkg.command == RPC_CMD_HEARTBEAT) {
        clt->last_heartbeat = current_timestamp();
        return;
    }
    clt->on_recv_pkg(ses, &pkg);
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
}

static void on_connect(nw_ses *ses, bool result)
{
    rpc_clt *clt= ses->privdata;
    if (result) {
        clt->curr_index = 0;
        clt->last_heartbeat = current_timestamp();
    }
    if (clt->on_connect) {
        clt->on_connect(ses, result);
    }
}

static int on_close(nw_ses *ses)
{
    rpc_clt *clt= ses->privdata;

    if (clt->curr_index == clt->addr_count) {
        clt->curr_index = 0;
        return 0;
    }
    memcpy(&ses->peer_addr, &clt->addr_arr[clt->curr_index], sizeof(nw_addr_t));
    clt->curr_index += 1;
    return 1;
}

static void on_timer(nw_timer *timer, void *privdata)
{
    rpc_clt *clt = privdata;
    if (!nw_clt_connected(clt->raw_clt))
        return;
    if (clt->raw_clt->ses.sock_type == SOCK_DGRAM)
        return;

    double now = current_timestamp();
    if (now - clt->last_heartbeat > RPC_HEARTBEAT_TIMEOUT) {
        nw_clt_close(clt->raw_clt);
        nw_clt_start(clt->raw_clt);
    } else {
        rpc_pkg pkg;
        memset(&pkg, 0, sizeof(pkg));
        pkg.pkg_type = RPC_PKG_TYPE_REQUEST;
        pkg.command = RPC_CMD_HEARTBEAT;
        rpc_send(&clt->raw_clt->ses, &pkg);
    }
}

rpc_clt *rpc_clt_create(rpc_clt_cfg *cfg, rpc_clt_type *type)
{
    if (cfg->name == NULL)
        return NULL;
    if (cfg->addr_count == 0)
        return NULL;
    if (type->on_recv_pkg == NULL)
        return NULL;

    nw_clt_cfg raw_cfg;
    memset(&raw_cfg, 0, sizeof(raw_cfg));
    memcpy(&raw_cfg.addr, &cfg->addr_arr[0], sizeof(nw_addr_t));
    raw_cfg.sock_type = cfg->sock_type;
    raw_cfg.read_mem = cfg->read_mem;
    raw_cfg.write_mem = cfg->write_mem;
    raw_cfg.reconnect_timeout = cfg->reconnect_timeout;
    raw_cfg.max_pkg_size = RPC_MAX_PKG_SIZE;

    nw_clt_type raw_type;
    memset(&raw_type, 0, sizeof(raw_type));
    raw_type.decode_pkg = rpc_decode;
    raw_type.on_connect = on_connect;
    raw_type.on_close = on_close;
    raw_type.on_recv_pkg = on_recv_pkg;
    raw_type.on_error_msg = on_error_msg;

    rpc_clt *clt = malloc(sizeof(rpc_clt));
    assert(clt != NULL);

    clt->raw_clt = nw_clt_create(&raw_cfg, &raw_type, clt);
    if (clt->raw_clt == NULL) {
        free(clt);
        return NULL;
    }

    clt->name = strdup(cfg->name);
    assert(clt->name != NULL);
    clt->addr_count = cfg->addr_count;
    clt->addr_arr = malloc(sizeof(nw_addr_t) * clt->addr_count);
    assert(clt->addr_arr != NULL);
    memcpy(clt->addr_arr, cfg->addr_arr, sizeof(nw_addr_t) * clt->addr_count);

    nw_timer_set(&clt->timer, RPC_HEARTBEAT_INTERVAL, true, on_timer, clt);
    clt->on_recv_pkg = type->on_recv_pkg;
    clt->on_connect = type->on_connect;

    return clt;
}

int rpc_clt_start(rpc_clt *clt)
{
    int ret = nw_clt_start(clt->raw_clt);
    if (ret < 0) {
        return ret;
    }
    nw_timer_start(&clt->timer);
    return 0;
}

int rpc_clt_close(rpc_clt *clt)
{
    int ret = nw_clt_close(clt->raw_clt);
    if (ret < 0) {
        return ret;
    }
    nw_timer_stop(&clt->timer);
    return 0;
}

int rpc_clt_send(rpc_clt *clt, rpc_pkg *pkg)
{
    return rpc_send(&clt->raw_clt->ses, pkg);
}

void rpc_clt_release(rpc_clt *clt)
{
    nw_clt_release(clt->raw_clt);
    nw_timer_stop(&clt->timer);
    free(clt);
}

bool rpc_clt_connected(rpc_clt *clt)
{
    return nw_clt_connected(clt->raw_clt);
}

