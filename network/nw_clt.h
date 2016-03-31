/*
 * Description: 
 *     History: yang@haipo.me, 2016/03/21, create
 */

# ifndef _NW_CLT_H_
# define _NW_CLT_H_

# include <stdint.h>
# include <stdbool.h>

# include "nw_buf.h"
# include "nw_evt.h"
# include "nw_ses.h"
# include "nw_sock.h"
# include "nw_timer.h"

typedef struct nw_clt_cfg {
    nw_addr_t addr;
    int sock_type;
    uint32_t max_pkg_size;
    uint32_t read_mem;
    uint32_t write_mem;
    double reconnect_timeout;
} nw_clt_cfg;

typedef struct nw_clt_type {
    int (*decode_pkg)(nw_ses *ses, void *data, size_t max);
    int (*on_close)(nw_ses *ses);
    void (*on_connect)(nw_ses *ses, bool result);
    void (*on_recv_pkg)(nw_ses *ses, void *data, size_t size);
    void (*on_recv_fd)(nw_ses *ses, int fd);
    void (*on_error_msg)(nw_ses *ses, const char *msg);
} nw_clt_type;

typedef struct nw_clt {
    nw_ses ses;
    nw_clt_type type;
    nw_buf_pool *buf_pool;
    nw_timer timer;
    bool connected;
    double reconnect_timeout;
    uint32_t read_mem;
    uint32_t write_mem;
} nw_clt;

nw_clt *nw_clt_create(nw_clt_cfg *cfg, nw_clt_type *type, void *privdata);
int nw_clt_start(nw_clt *clt);
int nw_clt_close(nw_clt *clt);
void nw_clt_release(nw_clt *clt);
bool nw_clt_connected(nw_clt *clt);

# endif

