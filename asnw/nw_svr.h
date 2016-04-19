/*
 * Description: 
 *     History: yang@haipo.me, 2016/03/20, create
 */

# ifndef _NW_SVR_H_
# define _NW_SVR_H_

# include <stdint.h>

# include "nw_buf.h"
# include "nw_evt.h"
# include "nw_ses.h"
# include "nw_sock.h"

typedef struct nw_svr_bind {
    nw_addr_t addr;
    int sock_type;
} nw_svr_bind;

typedef struct nw_svr_cfg {
    uint32_t bind_count;
    nw_svr_bind *bind_arr;
    uint32_t max_pkg_size;
    uint32_t read_mem;
    uint32_t write_mem;
} nw_svr_cfg;

typedef struct nw_svr_type {
    int (*decode_pkg)(nw_ses *ses, void *data, size_t max);
    int (*on_accept)(nw_ses *ses, int sockfd, nw_addr_t *peer_addr);
    void (*on_new_connection)(nw_ses *ses);
    void (*on_connection_close)(nw_ses *ses);
    void (*on_recv_pkg)(nw_ses *ses, void *data, size_t size);
    void (*on_recv_fd)(nw_ses *ses, int fd);
    void (*on_error_msg)(nw_ses *ses, const char *msg);
    void *(*on_privdata_alloc)(void *svr);
    void (*on_privdata_free)(void *svr, void *privdata);
} nw_svr_type;

typedef struct nw_svr {
    uint32_t svr_count;
    nw_ses *svr_list;
    nw_svr_type type;
    nw_buf_pool *buf_pool;
    nw_cache *clt_cache;
    nw_ses *clt_list_head;
    nw_ses *clt_list_tail;
    uint32_t clt_count;
    uint32_t read_mem;
    uint32_t write_mem;
    uint64_t id_start;
    void *privdata;
} nw_svr;

nw_svr *nw_svr_create(nw_svr_cfg *cfg, nw_svr_type *type, void *privdata);
int nw_svr_add_clt_fd(nw_svr *svr, int fd);
int nw_svr_start(nw_svr *svr);
int nw_svr_stop(nw_svr *svr);
void nw_svr_release(nw_svr *svr);
void nw_svr_close_clt(nw_svr *svr, nw_ses *ses);

# endif

