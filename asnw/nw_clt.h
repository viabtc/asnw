/*
 * Description: client, keeps a connection to one address
 *     History: yang@haipo.me, 2016/03/21, create
 */

# ifndef _NW_CLT_H_
# define _NW_CLT_H_

# include <stdint.h>
# include <stdbool.h>

# include "nw_buf.h"
# include "nw_evt.h"
# include "nw_ses.h"
# include "nw_dns.h"
# include "nw_sock.h"
# include "nw_timer.h"

/*
 * nw_clt is a client: it keeps a connection to one address and connects
 * again by itself whenever the connection is lost or can't be established.
 *
 * usage:
 *   fill a nw_clt_cfg (address via nw_clt_cfg_parse) and a nw_clt_type
 *   (callbacks, memset it to 0 first), then
 *   nw_clt_create -> nw_clt_start -> nw_loop_run ... -> nw_clt_release
 *   send with nw_ses_send(&clt->ses, data, size)
 *
 * all callbacks run on the loop thread. see example/ping_clt.c.
 *
 * a server given by name
 *
 * with nw_clt_cfg_parse the address may be a host name ("tcp@db.local:5432").
 * the name is resolved by nw_dns, off the loop thread, before the client
 * connects, and again each time it has to connect anew after a connection
 * was lost: a client follows its server when the name moves to another
 * address. a name with several addresses is tried address by address, in
 * the order the system gives them; that is one connect attempt, and only
 * when the last address has failed too is it reported (on_connect false,
 * on_close) and repeated reconnect_timeout later, starting with a fresh
 * look at the name. a name that can't be resolved is a failed attempt as
 * well, reported through on_error_msg and on_close. the callbacks see the
 * address in use in ses->peer_addr; it always comes from the lookup, a
 * change made to it in on_close is lost.
 *
 * a dgram client has no connection to lose, so its name is resolved once,
 * when the client is started: it sends to the first address until
 * nw_clt_start is called again, wherever the name points by then. it can't
 * send before the lookup is done, on_connect(true) tells when. a lookup
 * that fails is reported through on_error_msg only, and repeated
 * reconnect_timeout later.
 *
 * stream and seqpacket clients
 *
 * nw_clt_start only begins to connect, on_connect tells how it ended. do
 * not send before the client is connected (check nw_clt_connected): the
 * failed write is treated as a connection error. after a failure or a
 * disconnect the next attempt is made reconnect_timeout seconds later, and
 * so on forever; data still waiting to be sent is dropped.
 *
 * dgram clients
 *
 * there is no connection: the client is usable right after nw_clt_start,
 * on_connect is not called (and on_close only by nw_clt_stop), and
 * nothing tells whether the server is there. the exception is a server
 * given by name, see above: the client is usable once the name is
 * resolved, and on_connect(true) is called then. it always sends to the
 * configured addr, or to the one its name gave. udp datagrams arriving
 * from any other address are dropped, unless that addr is a broadcast,
 * multicast or wildcard address. a unix dgram client binds
 * a socket file of its own under /tmp so that the server can reply, and
 * removes it on close.
 */

typedef struct nw_clt_cfg {
    /* the server address */
    nw_addr addr;
    /* SOCK_STREAM, SOCK_DGRAM or SOCK_SEQPACKET */
    int sock_type;
    /* optional
     *
     * the server by name: if name is not empty, addr is not used and the
     * client connects to what name resolves to, on this port. sock_type
     * must be SOCK_STREAM or SOCK_DGRAM. nw_clt_cfg_parse fills these */
    char name[NW_DNS_MAX_NAME];
    uint16_t port;
    /* must not be 0
     *
     * max size of one message, in bytes, and the size of the bufs used for
     * reading and for the pending send list. with buf_pool set, the buf
     * size of that pool is what applies (this must still not be 0) */
    uint32_t max_pkg_size;
    /* max number of bufs in the pending send list, i.e. up to
     * buf_limit * max_pkg_size bytes can wait to be sent; nw_ses_send fails
     * beyond that. 0 means no limit */
    uint32_t buf_limit;
    /* kernel receive buffer size of the socket (SO_RCVBUF), 0 to keep the
     * system default */
    uint32_t read_mem;
    /* kernel send buffer size of the socket (SO_SNDBUF), 0 to keep the
     * system default */
    uint32_t write_mem;
    /* seconds to wait before connecting again after a failure or a
     * disconnect. also the time a connect attempt may take before it is
     * given up. 0 means the default, 1.0 */
    double reconnect_timeout;
    /* optional
     *
     * a buf pool to use instead of a private one, to share bufs between
     * many clients. its buf size takes the place of max_pkg_size. it must
     * outlive the client and is not released with it */
    nw_buf_pool *buf_pool;
} nw_clt_cfg;

typedef struct nw_clt_type {
    /* must
     *
     * tell where a message ends, same contract as nw_svr_type.decode_pkg:
     *
     * return > 0: the size of the first full message in data,
     * return = 0: no full message yet (stream: wait for more data;
     *             dgram, seqpacket: the packet is dropped),
     * return < 0: broken data (stream, seqpacket: the connection is closed
     *             and connected again; dgram: only reported). */
    int (*decode_pkg)(nw_ses *ses, void *data, size_t max);
    /* optional
     *
     * called once each time the connection is lost, a connect attempt
     * fails, or nw_clt_stop is called. the place to fail the requests that
     * were waiting for a reply, or to pick another server by changing
     * ses->peer_addr (not when the server was given by name: the address
     * then comes from the lookup).
     * return > 0 to connect again at once (on the next turn of the loop),
     * anything else to wait reconnect_timeout first. the return value is
     * ignored for nw_clt_stop.
     *
     * the connection is over when this is called: nw_ses_send returns -1.
     * calling nw_clt_stop from here means "stay closed": no reconnect is
     * scheduled */
    int (*on_close)(nw_ses *ses);
    /* optional
     *
     * called when everything that was queued has been written to the
     * socket, see on_drain in nw_svr_type */
    void (*on_drain)(nw_ses *ses);
    /* optional
     *
     * called when a connect attempt ends. result true: connected, the
     * client can send. result false: failed, errno holds the reason;
     * on_close is called next and the client tries again. an attempt that
     * times out is reported through on_close only. by name, an attempt is
     * all the addresses of the name: false comes once, after the last one.
     * a dgram client is only told, with true, when its server was given by
     * name: the lookup is done */
    void (*on_connect)(nw_ses *ses, bool result);
    /* must
     *
     * called for every full message, put your business logic here. data is
     * only valid during the call, copy what you need to keep */
    void (*on_recv_pkg)(nw_ses *ses, void *data, size_t size);
    /* optional
     *
     * called when a file descriptor arrives on a seqpacket connection.
     * the fd is yours, close it when done. if not set, it is closed */
    void (*on_recv_fd)(nw_ses *ses, int fd);
    /* optional
     *
     * called when something goes wrong, msg says what, for your log. on a
     * stream or seqpacket client on_close is called next and the client
     * connects again. msg is only valid during the call */
    void (*on_error_msg)(nw_ses *ses, const char *msg);
} nw_clt_type;

/* the fields are for reading only */
typedef struct nw_clt {
    /* the session, pass &clt->ses to nw_ses_send. must stay the first
     * member: the nw_ses given to the callbacks is this one */
    nw_ses ses;
    nw_clt_type type;
    bool custom_buf_pool;
    nw_buf_pool *buf_pool;
    /* connect timeout while connecting, reconnect delay while waiting */
    nw_timer timer;
    bool connected;
    bool on_connect_called;
    /* a unix dgram client binds a private socket file, removed on close */
    bool local_path_bound;
    /* a server given by name: the name, the lookup in progress if any, and
     * the addresses it gave, of which addr_next is the next one to try */
    char *name;
    uint16_t port;
    nw_dns_query *dns_query;
    nw_addr *addrs;
    uint32_t addr_count;
    uint32_t addr_next;
    /* look the name up again before the next connect */
    bool resolve_again;
    /* on_close is running; nw_clt_stop was called from inside it */
    bool in_on_close;
    bool close_requested;
    double reconnect_timeout;
    uint32_t read_mem;
    uint32_t write_mem;
} nw_clt;

/*
 * fill the address of a cfg from a string: what nw_sock_addr_parse takes,
 * and for tcp and udp also a host name in place of the ip:
 *
 *   tcp@10.0.0.1:5432       -> cfg->addr
 *   tcp@db.local:5432       -> cfg->name, cfg->port
 *   stream@/tmp/db.sock     -> cfg->addr
 *
 * the other fields of cfg are left alone. return 0, the errors of
 * nw_sock_addr_parse, or -6 if it is not a possible name (too long,
 * or other characters than letters, digits, '-', '_' and '.').
 */
int nw_clt_cfg_parse(const char *str, nw_clt_cfg *cfg);

/* create a client. cfg and type are copied. nothing is connected yet.
 * privdata is stored in clt->ses.privdata, i.e. it is ses->privdata in
 * every callback. return NULL if max_pkg_size is 0, a `must` callback is
 * missing or memory runs out */
nw_clt *nw_clt_create(nw_clt_cfg *cfg, nw_clt_type *type, void *privdata);

/* start the client: begin to connect (stream, seqpacket) or get ready to
 * send (dgram). on a client that is already started it drops the current
 * connection and starts over.
 * return 0 once the client is under way: also when the first attempt
 * failed on the spot, since it will be retried. return -1 if no socket
 * could be created or bound or, by name, the lookup could not be started;
 * nothing is retried then */
int nw_clt_start(nw_clt *clt);

/* close the connection and stay closed: on_close is called, pending data
 * is dropped and no reconnect is scheduled. nw_clt_start starts it again */
int nw_clt_stop(nw_clt *clt);

/* close without calling on_close, and free the client. don't call it from
 * a callback of this client */
void nw_clt_release(nw_clt *clt);

/* can the client send right now? stream, seqpacket: the connection is
 * established. dgram: the client is started and, by name, its lookup is
 * done */
bool nw_clt_connected(nw_clt *clt);

# endif
