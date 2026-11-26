/*
 * Description: network session, one socket with its buffers and callbacks
 *     History: yang@haipo.me, 2016/03/18, create
 */

# ifndef _NW_SES_H_
# define _NW_SES_H_

# include <stdbool.h>

# include "nw_buf.h"
# include "nw_evt.h"
# include "nw_sock.h"

/*
 * nw_ses is the low level object under nw_svr and nw_clt: one socket, its
 * read buffer, its pending send list and the callbacks that handle its
 * events.
 *
 * as a user you get a nw_ses in every callback. what you do with it:
 *   - read its fields: peer_addr, host_addr, id, privdata, svr, sock_type
 *   - send with nw_ses_send (and nw_ses_send_fd)
 *   - control the flow, if you need to: see "flow control" below
 * create, start and close sessions through nw_svr / nw_clt, not with the
 * nw_ses_* functions of the internal section below.
 */

enum {
    /* a connection accepted by a nw_svr */
    NW_SES_TYPE_COMMON,
    /* the session of a nw_clt */
    NW_SES_TYPE_CLIENT,
    /* a listening (stream, seqpacket) or bound (dgram) session of a nw_svr */
    NW_SES_TYPE_SERVER,
};

typedef struct nw_ses {
    /* the libev watcher of sockfd. must stay the first member */
    ev_io ev;
    /* the loop instance, nw_default_loop */
    struct ev_loop *loop;
    /* -1 when the session is closed */
    int sockfd;
    /* one of SOCK_STREAM, SOCK_DGRAM, SOCK_SEQPACKET */
    int sock_type;
    /* one of NW_SES_TYPE_COMMON, NW_SES_TYPE_CLIENT, NW_SES_TYPE_SERVER */
    int ses_type;
    /* the other end. connection: the remote address. nw_clt: the configured
     * server. dgram session of a nw_svr: the sender of the datagram being
     * processed, it changes with every datagram */
    nw_addr peer_addr;
    /* the local address. connections of a nw_svr share the pointer of the
     * session that accepted them */
    nw_addr *host_addr;
    /* data received and not consumed yet, NULL when there is none */
    nw_buf *read_buf;
    /* data accepted by nw_ses_send and not written to the socket yet */
    nw_buf_list *write_buf;
    nw_buf_pool *pool;
    /* connections of a nw_svr get an id that is unique within that server
     * and never 0. it is 0 for other sessions and after close. see the
     * session lifetime note in nw_svr.h */
    uint64_t id;
    /* user data. nw_svr connection: what on_privdata_alloc returned.
     * nw_clt: the privdata given to nw_clt_create */
    void *privdata;
    /* the nw_svr this session belongs to, NULL for a nw_clt session */
    void *svr;

    /* when data last arrived on this session, a time of the event loop
     * (ev_now). `ev_now(nw_default_loop) - ses->active_time` is how long
     * the peer has been silent */
    double active_time;
    /* timeouts of a nw_svr connection, managed by nw_svr: see
     * nw_svr_set_clt_idle_timeout and nw_svr_set_clt_deadline */
    double idle_timeout;
    double idle_mark;
    double deadline;
    double deadline_span;
    /* set by nw_svr_shutdown_clt: the session is on its way out. nothing
     * more can be sent on it and what it still receives is thrown away */
    bool closing;
    bool write_shut;
    double close_deadline;
    /* flow control, see nw_ses_pending and nw_ses_pause_read */
    size_t write_pending;
    bool read_paused;
    bool resume_decode;

    /* event handlers, installed by nw_svr / nw_clt */
    int  (*on_accept)(struct nw_ses *ses, int sockfd, nw_addr *peer_addr);
    int  (*decode_pkg)(struct nw_ses *ses, void *data, size_t max);
    void (*on_connect)(struct nw_ses *ses, bool result);
    void (*on_recv_pkg)(struct nw_ses *ses, void *data, size_t size);
    void (*on_recv_fd)(struct nw_ses *ses, int fd);
    void (*on_error)(struct nw_ses *ses, const char *msg);
    void (*on_close)(struct nw_ses *ses);
    /* optional, see on_drain in nw_svr_type */
    void (*on_drain)(struct nw_ses *ses);
} nw_ses;

/*
 * for users
 */

/*
 * send one message.
 *
 * stream: what the socket does not take right now is kept in the pending
 * send list and written when the socket becomes writable.
 * seqpacket: the packet is sent whole, or queued whole. size must not
 * exceed max_pkg_size.
 * dgram: sent to ses->peer_addr at once, never queued.
 *
 * return 0 if the data was sent or queued (it does not tell which).
 * return -1 on failure: the session is closed or shutting down
 * (nw_svr_shutdown_clt), the pending send list is full (buf_limit), or the
 * socket reported an error. in the last two cases the error handler has
 * run by then (on_error_msg), and for a connection that means it is closed
 * already: don't use ses after a -1.
 */
int nw_ses_send(nw_ses *ses, const void *data, size_t size);

/* pass an open file descriptor to the peer process, only on a seqpacket
 * connection. the peer gets it in on_recv_fd; the caller keeps its own fd
 * and still has to close it. not queued: return > 0 on success, -1 on
 * error (including EAGAIN when the socket buffer is full) */
int nw_ses_send_fd(nw_ses *ses, int fd);

/*
 * flow control
 *
 * by default there is none, and none is needed when a peer that falls
 * behind is simply dropped: nw_ses_send queues what the socket does not
 * take, and a queue that reaches buf_limit closes the connection.
 *
 * when dropping is not what you want (a proxy, a relay, a big download)
 * the library gives you the facts and the switches, and the policy is
 * yours:
 *
 *   nw_ses_pending      how much is waiting to be written
 *   on_drain            a callback of nw_svr / nw_clt: all of it is out
 *   nw_ses_pause_read   stop taking input from a session
 *   nw_ses_resume_read  take it again
 *
 * e.g. to pass data from `up` to `down` without piling it up: after
 * sending to down, pause up if nw_ses_pending(down) is over your mark;
 * in on_drain of down, resume up. the kernel then makes the sender wait.
 */

/* bytes accepted by nw_ses_send and not written to the socket yet. 0 for a
 * dgram session, which never queues */
size_t nw_ses_pending(nw_ses *ses);

/*
 * stop reading from a session: no more on_recv_pkg for it, the data stays
 * in the kernel (where it slows the sender down once the socket buffer is
 * full) and, for a stream, whatever was already read stays in the session.
 * called from on_recv_pkg, it takes effect at once: the messages behind
 * the current one are not delivered. sending is not affected.
 *
 * a paused session does not notice that its peer closed the connection or
 * went silent until it is resumed, or until a send to it fails. and it
 * still times out: being paused counts as silence for the idle timeout, so
 * give it a timeout of its own (nw_svr_set_clt_idle_timeout) if it may be
 * paused for long.
 *
 * for connections and nw_clt sessions that are connected; nothing happens
 * for others, for a session that is paused already or shutting down.
 */
void nw_ses_pause_read(nw_ses *ses);

/* read again. what was held back is delivered from the next turn of the
 * event loop on, not from inside this call. nothing happens if the
 * session is not paused */
void nw_ses_resume_read(nw_ses *ses);

/*
 * internal, used by nw_svr and nw_clt
 */

/* fill in a session. sockfd is set to -1, the caller sets sockfd, sock_type,
 * the addresses and the handlers afterwards. return 0, or -1 if out of memory */
int nw_ses_init(nw_ses *ses, struct ev_loop *loop, nw_buf_pool *pool, uint32_t buf_limit, int ses_type);
/* bind to addr. for a unix address a stale socket file at the path is
 * removed first and the new one is made accessible to everybody (0777) */
int nw_ses_bind(nw_ses *ses, nw_addr *addr);
/* listen and start accepting, on_accept is called for every new connection */
int nw_ses_listen(nw_ses *ses, int backlog);
/* start a non blocking connect. return 0 if it succeeded (on_connect(true)
 * was called) or is in progress (on_connect is called later). return -1 if
 * it failed on the spot, errno is set and on_connect is NOT called */
int nw_ses_connect(nw_ses *ses, nw_addr *addr);
/* start handling events: accept for a listening session, read otherwise */
int nw_ses_start(nw_ses *ses);
/* stop handling events, the socket stays open */
int nw_ses_stop(nw_ses *ses);
/* begin an orderly close: no more sends, input is discarded from now on,
 * and the sending side of the socket is shut down as soon as everything
 * queued has been written. the socket itself stays open, the owner closes
 * it when the peer hangs up or after a wait of its choice */
void nw_ses_shutdown(nw_ses *ses);
/* close the socket and drop buffered data. the session can be used again */
int nw_ses_close(nw_ses *ses);
/* close, and free what nw_ses_init allocated */
int nw_ses_release(nw_ses *ses);

# endif
