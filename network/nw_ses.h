/*
 * Description: network session
 *     History: yang@haipo.me, 2016/03/18, create
 */

# ifndef _NW_SES_H_
# define _NW_SES_H_

# include <stdbool.h>

# include "nw_buf.h"
# include "nw_evt.h"
# include "nw_sock.h"

enum {
    NW_SES_TYPE_COMMON, /* stream connection */
    NW_SES_TYPE_CLIENT, /* server side */
    NW_SES_TYPE_SERVER, /* client side */
};

typedef struct nw_ses {
    ev_io ev;
    struct ev_loop *loop;
    int sockfd;
    int sock_type;
    int ses_type;
    bool connected;
    nw_addr_t peer_addr;
    nw_addr_t *host_addr;
    nw_buf *read_buf;
    nw_buf_list *write_buf;
    nw_buf_pool *pool;
    uint32_t id;
    void *privdata;
    void *svr;

    struct nw_ses *prev;
    struct nw_ses *next;

    int  (*on_accept)(struct nw_ses *ses, int sockfd, nw_addr_t *peer_addr);
    int  (*decode_pkg)(struct nw_ses *ses, void *data, size_t max);
    void (*on_connect)(struct nw_ses *ses, bool result);
    void (*on_recv_pkg)(struct nw_ses *ses, void *data, size_t size);
    void (*on_recv_fd)(struct nw_ses *ses, int fd);
    void (*on_error)(struct nw_ses *ses, const char *msg);
    void (*on_close)(struct nw_ses *ses);
} nw_ses;

int nw_ses_bind(nw_ses *ses, nw_addr_t *addr);
int nw_ses_listen(nw_ses *ses, int backlog);
int nw_ses_connect(nw_ses *ses, nw_addr_t *addr);
int nw_ses_start(nw_ses *ses);
int nw_ses_stop(nw_ses *ses);
int nw_ses_send(nw_ses *ses, const void *data, size_t size);
int nw_ses_send_fd(nw_ses *ses, int fd);

int nw_ses_init(nw_ses *ses, struct ev_loop *loop, nw_buf_pool *pool, int ses_type);
int nw_ses_close(nw_ses *ses);
int nw_ses_release(nw_ses *ses);

# endif

