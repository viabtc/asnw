/*
 * Description: 
 *     History: yang@haipo.me, 2016/03/30, create
 */

# include <stdlib.h>
# include <assert.h>
# include <endian.h>
# include <byteswap.h>
# include <sys/time.h>
# include <unistd.h>

# include "ut_rpc.h"
# include "ut_crc32.h"

# if __BYTE_ORDER == __LITTLE_ENDIAN
#  define htole16(x) (x)
#  define htole32(x) (x)
#  define htole64(x) (x)
#  define le16toh(x) (x)
#  define le32toh(x) (x)
#  define le64toh(x) (x)
# else
#  define htole16(x) bswap_16(x)
#  define htole32(x) bswap_32(x)
#  define htole64(x) bswap_64(x)
#  define le16toh(x) bswap_16(x)
#  define le32toh(x) bswap_32(x)
#  define le64toh(x) bswap_64(x)
# endif

int rpc_decode(nw_ses *ses, void *data, size_t max)
{
    if (max < RPC_PKG_HEAD_SIZE)
        return 0;

    rpc_pkg *pkg = data;
    if (le32toh(pkg->magic) != RPC_PKG_MAGIC)
        return -1;
    uint32_t pkg_size = RPC_PKG_HEAD_SIZE + le16toh(pkg->ext_size) + le32toh(pkg->body_size);
    if (pkg_size > RPC_MAX_PKG_SIZE)
        return -2;
    if (max < pkg_size)
        return 0;

    uint32_t crc32 = le32toh(pkg->crc32);
    pkg->crc32 = 0;
    if (crc32 != generate_crc32c(data, pkg_size))
        return -3;
    pkg->crc32 = crc32;

    pkg->magic     = le32toh(pkg->magic);
    pkg->command   = le32toh(pkg->command);
    pkg->pkg_type  = le16toh(pkg->pkg_type);
    pkg->result    = le32toh(pkg->result);
    pkg->sequence  = le32toh(pkg->sequence);
    pkg->req_id    = le64toh(pkg->req_id);
    pkg->body_size = le32toh(pkg->body_size);
    pkg->ext_size  = le16toh(pkg->ext_size);

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
    pkg->magic     = htole32(RPC_PKG_MAGIC);
    pkg->command   = htole32(pkg->command);
    pkg->pkg_type  = htole16(pkg->pkg_type);
    pkg->result    = htole32(pkg->result);
    pkg->sequence  = htole32(pkg->sequence);
    pkg->req_id    = htole64(pkg->req_id);
    pkg->body_size = htole32(pkg->body_size);
    pkg->ext_size  = htole16(pkg->ext_size);

    pkg->crc32 = 0;
    pkg->crc32 = htole32(generate_crc32c(send_buf, pkg_size));

    return nw_ses_send(ses, send_buf, pkg_size);
}

double current_timestamp(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + tv.tv_usec / 1000000.0;
}

