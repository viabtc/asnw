/*
 * Description: 
 *     History: yang@haipo.me, 2016/03/30, create
 */

# include <stdlib.h>
# include <assert.h>

# include "ut_rpc.h"
# include "ut_crc32.h"

int rpc_decode(nw_ses *ses, void *data, size_t max)
{
    if (max < RPC_PKG_HEAD_SIZE)
        return 0;
    rpc_pkg *pkg = data;
    if (pkg->magic != RPC_PKG_MAGIC) {
        return -1;
    }
    uint32_t pkg_size = RPC_PKG_HEAD_SIZE + pkg->ext_size + pkg->body_size;
    if (pkg_size > RPC_MAX_PKG_SIZE)
        return -2;
    if (max < pkg_size)
        return 0;
    uint32_t crc32 = pkg->crc32;
    pkg->crc32 = 0;
    if (crc32 != generate_crc32c(data, pkg_size)) {
        return -3;
    }
    pkg->crc32 = crc32;
    return pkg_size;
}

int rpc_send(nw_ses *ses, rpc_pkg *pkg)
{
    static void *send_buf;
    if (send_buf == NULL) {
        send_buf = malloc(RPC_MAX_PKG_SIZE);
        assert(send_buf != NULL);
    }
    uint32_t pkg_size = RPC_PKG_HEAD_SIZE + pkg->ext_size + pkg->body_size;
    if (pkg_size > RPC_MAX_PKG_SIZE)
        return -1;
    memcpy(send_buf, pkg, RPC_PKG_HEAD_SIZE);
    if (pkg->ext_size)
        memcpy(send_buf + RPC_PKG_HEAD_SIZE, pkg->ext, pkg->ext_size);
    if (pkg->body_size)
        memcpy(send_buf + RPC_PKG_HEAD_SIZE + pkg->ext_size, pkg->body, pkg->body_size);
    pkg = send_buf;
    pkg->magic = RPC_PKG_MAGIC;
    pkg->crc32 = 0;
    pkg->crc32 = generate_crc32c(send_buf, pkg_size);
    return nw_ses_send(ses, send_buf, pkg_size);
}

