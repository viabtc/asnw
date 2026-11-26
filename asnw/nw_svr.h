/*
 * Description: server, listens on one or more addresses
 *     History: yang@haipo.me, 2016/03/20, create
 */

# ifndef _NW_SVR_H_
# define _NW_SVR_H_

# include <stdint.h>

# include "nw_buf.h"
# include "nw_dict.h"
# include "nw_evt.h"
# include "nw_ses.h"
# include "nw_sock.h"
# include "nw_timer.h"

/*
 * nw_svr is a server: it listens on one or more addresses, which may be of
 * different types (tcp, udp, unix stream / dgram / seqpacket), and handles
 * all of them with the same set of callbacks.
 *
 * usage:
 *   fill a nw_svr_cfg (addresses via nw_sock_addr_parse) and a nw_svr_type
 *   (callbacks, memset it to 0 first), then
 *   nw_svr_create -> nw_svr_start -> nw_loop_run ... -> nw_svr_release
 *
 * all callbacks run on the loop thread. see example/echo_server.c.
 *
 * session lifetime
 *
 * a nw_ses passed to a callback is only valid until that connection is
 * closed. the memory is recycled for later connections, so a nw_ses pointer
 * kept across asynchronous work (nw_job, nw_state, a timer ...) may by then
 * belong to a different client.
 *
 * so don't keep the pointer, keep ses->id: ids are unique per server, never
 * 0 and never reused. when the work is done, ask for the connection again:
 *
 *   nw_ses *ses = nw_svr_get(svr, saved_id);
 *   if (ses == NULL)
 *       return;        (the client is gone)
 *   nw_ses_send(ses, ...);
 *
 * or use nw_svr_send(svr, saved_id, ...), which does the same.
 *
 * timeouts and closing
 *
 * idle_timeout in the cfg closes connections that stay silent. a
 * connection can also be given a timeout of its own or a deadline
 * (nw_svr_set_idle_timeout, nw_svr_set_deadline), and on_timeout
 * lets you decide what a timeout means. all of it is checked by one timer
 * that walks the connections about once a second, so timeouts are
 * accurate to about a second (the timer runs faster while a connection
 * has a timeout shorter than two seconds, and slows down again when that
 * one is gone), and nothing runs when no connection uses them.
 *
 * nw_svr_close closes a connection at once, dropping what was not
 * sent yet. to say something last and be sure it arrives, send it and
 * call nw_svr_shutdown.
 *
 * memory
 *
 * a connection with unfinished input holds one buf of max_pkg_size bytes,
 * and what waits to be sent is kept in such bufs. they come from the pool
 * of the server, which reuses them, as its cache reuses the memory of the
 * sessions. once a minute the server gives what it has no use for back to
 * the system: it keeps as many free bufs as it has in use, and at least
 * 64, and the same for sessions. so the memory of a peak is returned
 * within a minute or two, a server under steady load keeps what it works
 * with, and a small one is left alone. there is nothing to set and
 * nothing to call.
 *
 * one more reason not to keep a nw_ses pointer: the memory of a closed
 * session is reused by a later connection, or given back to the system.
 *
 * datagrams
 *
 * a dgram bind has no connections: on_open / on_close
 * are not called and no privdata is allocated. the nw_ses in on_recv_pkg is
 * the bound session itself, and nw_ses_send replies to the sender of the
 * datagram being processed; reply inside the callback, or copy
 * ses->peer_addr if you need it later.
 */

typedef struct nw_svr_bind {
    /* the address to listen on */
    nw_addr addr;
    /* SOCK_STREAM, SOCK_DGRAM or SOCK_SEQPACKET */
    int sock_type;
} nw_svr_bind;

typedef struct nw_svr_cfg {
    /* number of entries in binds, at least 1 */
    uint32_t bind_count;
    /* the addresses to listen on. copied by nw_svr_create */
    nw_svr_bind *binds;
    /* must not be 0
     *
     * max size of one message, in bytes. it is also the size of the bufs:
     * every connection with unfinished input holds one, and the pending
     * send list is made of them. a stream connection sending a message
     * bigger than this is closed */
    uint32_t max_pkg_size;
    /* max number of bufs in the pending send list of a connection, i.e. a
     * connection can have up to buf_limit * max_pkg_size bytes waiting to
     * be sent; nw_ses_send fails beyond that.
     * 0 means no limit: a peer that stops reading can then hold any amount
     * of memory. set a limit for servers exposed to untrusted clients */
    uint32_t buf_limit;
    /* kernel receive buffer size of each socket (SO_RCVBUF), 0 to keep the
     * system default */
    uint32_t read_mem;
    /* kernel send buffer size of each socket (SO_SNDBUF), 0 to keep the
     * system default */
    uint32_t write_mem;
    /* optional
     *
     * seconds a connection may stay silent, i.e. send nothing at all,
     * before it times out: on_timeout is called if set, otherwise the
     * connection is closed. 0 for no timeout */
    double idle_timeout;
} nw_svr_cfg;

typedef struct nw_svr_type {
    /* must
     *
     * tell where a message ends. data points to `max` bytes received and
     * not consumed yet.
     *
     * return > 0: the size of the first full message, on_recv_pkg is then
     *             called with exactly that many bytes,
     * return = 0: the data does not hold a full message yet,
     * return < 0: broken data.
     *
     * a stream has no message boundaries, so this is where your framing
     * lives (a length header, a delimiter ...). it is called again for the
     * rest of the data, and again when more arrives. 0 waits for more data;
     * < 0 closes the connection.
     *
     * for dgram and seqpacket every packet is one message, max is the
     * packet size and you normally return it after checking the content.
     * a packet for which 0 is returned is dropped; < 0 closes a seqpacket
     * connection and is only reported (on_error_msg) for dgram.
     *
     * must not close the session or send on it */
    int (*decode_pkg)(nw_ses *ses, void *data, size_t max);
    /* optional
     *
     * called for every connection accepted on a stream or seqpacket bind,
     * in place of the default action, which is to add the connection to the
     * server. ses is the listening session, sockfd the new connection,
     * non-blocking and close-on-exec.
     * return < 0 and sockfd is closed for you; otherwise sockfd is yours:
     * hand it to another process (nw_ses_send_fd), or close it.
     * see example/listener_worker.c */
    int (*on_accept)(nw_ses *ses, int sockfd, nw_addr *peer_addr);
    /* optional
     *
     * called when a connection has been added to the server and can be
     * used. ses->id, ses->peer_addr and ses->privdata are set */
    void (*on_open)(nw_ses *ses);
    /* optional
     *
     * called when a connection is closed, whatever the reason: the peer
     * closed, an error, a timeout, nw_svr_close or nw_svr_release. the
     * last callback for that connection, called exactly once.
     *
     * ses can still be read (id, peer_addr, privdata) to clean up after
     * it, but the connection is already out of the server: ses_count no
     * longer counts it, nw_svr_get does not find it, the iterator does
     * not visit it, nw_ses_send on it returns -1 and closing it again does
     * nothing. everything else is allowed, including sending to and
     * closing other connections */
    void (*on_close)(nw_ses *ses);
    /* must
     *
     * called for every full message, put your business logic here. data is
     * only valid during the call, copy what you need to keep. the callback
     * may send, and may close the connection (nw_svr_close) */
    void (*on_recv_pkg)(nw_ses *ses, void *data, size_t size);
    /* optional
     *
     * called when a file descriptor arrives on a seqpacket connection.
     * the fd is yours, close it when done; it is close-on-exec. if not
     * set, it is closed */
    void (*on_recv_fd)(nw_ses *ses, int fd);
    /* optional
     *
     * called when something goes wrong, msg says what, for your log.
     * for a connection (read / write error, broken data, send list full) it
     * is followed by on_close and the connection is closed. for
     * a listening or dgram session (accept or sendto error) the server
     * keeps running. msg is only valid during the call */
    void (*on_error_msg)(nw_ses *ses, const char *msg);
    /* optional
     *
     * called when a connection has been silent for its idle timeout, or
     * when its deadline has passed. without this callback the connection
     * is closed (on_error_msg and on_close are called).
     *
     * with it, nothing else happens: close the connection here
     * (nw_svr_close, or nw_svr_shutdown after a last message), or
     * leave it open. left open and still silent, it times out again one
     * idle timeout later; ev_now(nw_default_loop) - ses->active_time
     * tells for how long it has been silent in total. that makes
     * "ping after 20 seconds, give up after 60" a few lines.
     *
     * the callback may close this connection, and others too */
    void (*on_timeout)(nw_ses *ses);
    /* optional
     *
     * called when everything that was queued for a connection has been
     * written to its socket: nw_ses_pending is 0 again. only when
     * something had to wait, not after a send that went out at once. for
     * flow control (see nw_ses.h) and for sending something big piece by
     * piece. the callback may send, and may close the connection */
    void (*on_drain)(nw_ses *ses);
    /* optional, if set on_privdata_free must be set too
     *
     * called for every new connection, before on_open. the
     * return value becomes ses->privdata: the place for your per connection
     * state. svr is the nw_svr. returning NULL rejects the connection */
    void *(*on_privdata_alloc)(void *svr);
    /* optional
     *
     * called when a connection is closed, after on_close, to
     * free what on_privdata_alloc returned */
    void (*on_privdata_free)(void *svr, void *privdata);
} nw_svr_type;

/* the fields are for reading only */
/* lives on the stack of the caller, see nw_svr_iter_init */
typedef struct nw_svr_iterator {
    nw_dict_iterator iter;
} nw_svr_iterator;

typedef struct nw_svr {
    /* the listening / bound sessions, one per bind, in cfg order */
    uint32_t bind_count;
    nw_ses *binds;
    nw_svr_type type;
    nw_buf_pool *buf_pool;
    nw_cache *ses_cache;
    /* the current connections by id, see nw_svr_get and nw_svr_next */
    nw_dict *ses_dict;
    /* number of current connections */
    uint32_t ses_count;
    uint32_t buf_limit;
    uint32_t read_mem;
    uint32_t write_mem;
    double idle_timeout;
    /* checks timeouts and finishes shutdowns, runs only while needed */
    nw_timer timer;
    double timer_interval;
    bool in_sweep;
    double sweep_shortest;
    /* once a minute, gives the bufs and the sessions the server has no
     * use for back to the system */
    nw_timer shrink_timer;
    uint64_t id_start;
    /* the privdata given to nw_svr_create. reach it from a callback with
     * ((nw_svr *)ses->svr)->privdata */
    void *privdata;
} nw_svr;

/* create a server. cfg and type are copied. the sockets are created here
 * but nothing is bound yet. privdata is stored in nw_svr privdata.
 * return NULL if cfg or type is incomplete (no bind, max_pkg_size 0, a
 * `must` callback missing), a socket can't be created or memory runs out */
nw_svr *nw_svr_create(nw_svr_cfg *cfg, nw_svr_type *type, void *privdata);

/* bind every address and start accepting / receiving. return 0, or -1 with
 * errno set if an address can't be bound (in use, no permission ...); the
 * binds before the failing one are then already active */
int nw_svr_start(nw_svr *svr);

/* stop accepting new connections and receiving datagrams. existing
 * connections keep working. a stopped server can't be started again */
int nw_svr_stop(nw_svr *svr);

/* close every connection (on_close and on_privdata_free are
 * called for each), close the listening sockets and free the server.
 * don't call it from a callback of this server */
void nw_svr_release(nw_svr *svr);

/* close a connection from the server side. on_close and
 * on_privdata_free are called. ses is invalid afterwards. may be called
 * from a callback, including for the session being handled */
void nw_svr_close(nw_svr *svr, nw_ses *ses);

/*
 * close a connection in an orderly way, for when the last thing you sent
 * has to arrive: an error response, a goodbye message.
 *
 * from this call on nothing more can be sent on the connection
 * (nw_ses_send returns -1) and what it still receives is thrown away, its
 * callbacks are no longer called. what was already queued is written
 * out, then the peer is told that nothing more will come. the connection
 * is closed for good when the peer hangs up in turn, or after 2 seconds;
 * on_close is called then, as for any close.
 *
 * why not just nw_svr_close: closing a socket that still has unread
 * input makes the kernel reset the connection, and a reset can destroy
 * data the peer has not read yet - typically the very message that
 * explains why it is being disconnected.
 */
void nw_svr_shutdown(nw_svr *svr, nw_ses *ses);

/* give one connection an idle timeout of its own, in place of the one of
 * the cfg. 0 turns it off for that connection, e.g. while it is waiting
 * for an answer that may take long. the silence is counted from the last
 * data received, or from this call if that is later */
void nw_svr_set_idle_timeout(nw_svr *svr, nw_ses *ses, double timeout);

/* make a connection time out `timeout` seconds from now whatever it does
 * in the meantime, unlike the idle timeout, which every byte received
 * pushes back. for things that have to be finished in a given time: a
 * handshake, a request that arrives in small pieces. it fires once.
 * 0 cancels it */
void nw_svr_set_deadline(nw_svr *svr, nw_ses *ses, double timeout);

/* find a current connection by its id (ses->id). return NULL if there is no
 * such connection: it was closed, or the id never existed. this is the safe
 * way to get back to a connection after asynchronous work, see the session
 * lifetime note above. the result is valid until that connection is closed,
 * don't keep it either */
nw_ses *nw_svr_get(nw_svr *svr, uint64_t id);

/* send to a connection by id: nw_svr_get + nw_ses_send. return 0, or -1
 * if the connection is gone or nw_ses_send failed (see there) */
int nw_svr_send(nw_svr *svr, uint64_t id, const void *data, size_t size);

/*
 * walk over the current connections of a server, in no particular order:
 *
 *   nw_svr_iterator iter;
 *   nw_ses *ses;
 *   nw_svr_iter_init(&iter, svr);
 *   while ((ses = nw_svr_next(&iter)) != NULL) {
 *       ...
 *   }
 *
 * connections may be closed while walking, the one just returned or any
 * other (say, by a close callback that tells the others and finds one of
 * them broken): the ones that are gone are skipped. the only thing that
 * invalidates the iterator is a new connection, which can't appear inside
 * the loop unless you call nw_svr_add_fd.
 * dgram binds have no connections and are not visited.
 */
void nw_svr_iter_init(nw_svr_iterator *iter, nw_svr *svr);
/* the next connection, NULL when all were visited */
nw_ses *nw_svr_next(nw_svr_iterator *iter);

/* add an already connected socket to the server as if it had been accepted
 * here, e.g. a fd received from another process in on_recv_fd. it is made
 * non-blocking and close-on-exec. the server must have a bind of the same
 * address family. return 0, or -1 on failure,
 * in which case fd is still yours to close */
int nw_svr_add_fd(nw_svr *svr, int fd);

/* same for a connection that was accepted somewhere else on the address of
 * bind number bind_index (in cfg order) of an identically configured
 * server: it becomes a connection of that bind here. peer_addr is what
 * accept said. fd has to be non-blocking, as the one given to on_accept
 * is. this is how nw_hub moves connections to its threads.
 * return 0, or -1 on failure, in which case fd is still yours to close */
int nw_svr_add_accepted_fd(nw_svr *svr, uint32_t bind_index, int fd, nw_addr *peer_addr);

# endif
