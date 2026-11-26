/*
 * Description: websocket server (RFC 6455) on top of nw_svr
 *     History: 2026/11/25, create
 */

# ifndef _NW_WS_H_
# define _NW_WS_H_

# include <stdint.h>
# include <stddef.h>
# include <stdbool.h>

# include "nw_buf.h"
# include "nw_svr.h"
# include "nw_hub.h"
# include "nw_http.h"

/*
 * nw_ws_svr is a websocket server: clients connect to its tcp or unix
 * stream binds, upgrade with the usual http handshake, and from then on
 * both sides exchange messages.
 *
 * usage:
 *   fill a nw_ws_svr_cfg and a nw_ws_svr_type (memset them to 0 first),
 *   nw_ws_svr_create -> nw_ws_svr_start -> nw_loop_run
 *   messages arrive in on_message, send with nw_ws_send_text and friends
 *
 * see test/ws_server.c.
 *
 * the life of a connection
 *
 *   connect      nothing is called yet
 *   handshake    on_upgrade (optional): look at the request, accept or refuse
 *                on_open (optional): the connection is a websocket now
 *   messages     on_message, any number of times
 *   close        on_close (optional), only if the handshake succeeded
 *
 * a port serves websocket only. a request that is not a valid upgrade gets
 * an http error (400, 403, 426) and the connection is closed.
 *
 * reading is zero copy
 *
 * a message that arrives in one frame - nearly all of them - is handed to
 * on_message where it lies in the read buffer, unmasked in place. only a
 * message split into several frames is put together in a buffer of its
 * own. either way the data is '\0' terminated and only valid during
 * on_message.
 *
 * sending later
 *
 * to push to a client outside of a callback for that client, keep ses->id
 * and use the _id functions; they do nothing if the client is gone. see the
 * session lifetime note in nw_svr.h.
 *
 * several threads
 *
 * with thread_count in the cfg the server runs on a nw_hub: the thread
 * that creates it only accepts, and every connection lives on one of
 * thread_count worker threads, where all its callbacks are called. see
 * nw_hub.h for what that means for your own data.
 *
 * each worker has a nw_ws_svr of its own, which is what nw_ws_svr_of
 * returns in a callback; its raw_svr holds the connections of that thread
 * and its privdata is the one given to nw_ws_svr_create. everything that
 * takes a nw_ses is used as before, on the thread of that connection. the
 * functions that take a server and an id (the _id functions, the
 * broadcasts) may be called from any thread, with the server returned by
 * nw_ws_svr_create or with the one of any worker: they act at once for a
 * connection of the calling thread and are queued for the others.
 *
 * what is not supported
 *
 *   - TLS (wss): put a reverse proxy in front
 *   - compression (permessage-deflate): not negotiated, clients fall back
 *   - checking that text messages are valid UTF-8
 *   - messages bigger than max_message_size: the connection is closed with
 *     code 1009
 *   - plain http requests on the same port
 */

/* close codes of RFC 6455, for nw_ws_close */
# define NW_WS_CLOSE_NORMAL         1000
# define NW_WS_CLOSE_GOING_AWAY     1001
# define NW_WS_CLOSE_PROTOCOL_ERROR 1002
# define NW_WS_CLOSE_UNSUPPORTED    1003
# define NW_WS_CLOSE_POLICY         1008
# define NW_WS_CLOSE_TOO_BIG        1009
# define NW_WS_CLOSE_SERVER_ERROR   1011

typedef struct nw_ws_svr nw_ws_svr;

typedef struct nw_ws_svr_cfg {
    /* number of entries in bind_arr, at least 1 */
    uint32_t bind_count;
    /* the addresses to listen on, all SOCK_STREAM (tcp or unix stream) */
    nw_svr_bind *bind_arr;
    /* max size of a message in bytes, whether it comes in one frame or in
     * several. it is also the size of the read buffer of a connection.
     * 0 for the default, 64K. at most 1G */
    uint32_t max_message_size;
    /* max number of bufs (of about max_message_size bytes) of outgoing data
     * waiting on a connection, 0 for no limit. a client that reads too
     * slowly to stay under the limit is disconnected. see nw_svr_cfg */
    uint32_t buf_limit;
    /* kernel receive and send buffer size of each connection (SO_RCVBUF,
     * SO_SNDBUF), 0 to keep the system default. see nw_svr_cfg */
    uint32_t read_mem;
    uint32_t write_mem;
    /* seconds a connection may stay silent before it is closed. anything
     * the client sends counts, including the pong to our ping.
     * 0 for the default, 60 */
    double keep_alive_timeout;
    /* send a ping to a connection that has been silent for this many
     * seconds, so that a live client answers and is not timed out. use a
     * value well below keep_alive_timeout. 0 to send no pings: clients
     * then have to send something by themselves often enough */
    double ping_interval;
    /* optional
     *
     * the subprotocol this server speaks. a client that offers a list of
     * subprotocols is accepted only if this one is in it, and gets it
     * confirmed. a client that offers none is accepted as it is. copied */
    const char *protocol;
    /* optional
     *
     * the origin to accept, to keep pages of other sites from connecting
     * with a user's cookies. give a host name: "example.com" accepts the
     * origins example.com and anything ending in .example.com, on any
     * scheme and port. or give a full origin ("https://app.example.com"),
     * which has to match exactly. clients that send no Origin header are
     * refused. NULL accepts everybody. copied */
    const char *origin;
    /* optional
     *
     * 0: no threads, everything runs on the thread that calls nw_loop_run.
     * 1 and more: that many worker threads, see "several threads" above */
    uint32_t thread_count;
    /* with threads: max bytes that may wait for one worker thread, see
     * nw_hub_cfg. 0 means no limit */
    size_t queue_limit;
} nw_ws_svr_cfg;

typedef struct nw_ws_svr_type {
    /* optional
     *
     * called when a valid upgrade request arrives, before it is answered.
     * look at req (path, query, headers: a token, a cookie, the address a
     * proxy forwarded ...) and return 0 to accept the client, < 0 to
     * refuse it with 403.
     *
     * req is only valid during the call: copy what you need later into
     * your privdata (nw_ws_privdata works here). don't send from here, the
     * handshake is not answered yet; use on_open */
    int (*on_upgrade)(nw_ses *ses, nw_http_request *req);
    /* optional
     *
     * called when the handshake is done. the place to send a first message */
    void (*on_open)(nw_ses *ses);
    /* must
     *
     * called for every message. binary tells a binary message from a text
     * one. data[size] is '\0', so a text message is a C string. data is
     * only valid during the call. the callback may send, and may close the
     * connection */
    void (*on_message)(nw_ses *ses, bool binary, void *data, size_t size);
    /* optional
     *
     * called once when a connection that completed the handshake is
     * closed, whatever the reason. nw_ws_privdata is still valid. the
     * connection itself is already out of the server: sending to it
     * returns -1 and a broadcast does not include it, so "tell the others
     * that this one left" is simply a broadcast from here */
    void (*on_close)(nw_ses *ses);
    /* optional
     *
     * called when something goes wrong, msg says what, for your log */
    void (*on_error_msg)(nw_ses *ses, const char *msg);
    /* optional, if set on_privdata_free must be set too
     *
     * called during the handshake, before on_upgrade. the return value is
     * the per connection data of the application, see nw_ws_privdata. svr
     * is the nw_ws_svr. returning NULL refuses the client */
    void *(*on_privdata_alloc)(void *svr);
    /* optional
     *
     * called to free what on_privdata_alloc returned: after on_close, or
     * right away if on_upgrade refused the client */
    void (*on_privdata_free)(void *svr, void *privdata);
    /* optional, see nw_svr_type: the messages queued for a connection
     * have all been written to its socket. only called once the handshake
     * is done. nw_ses_pending, nw_ses_pause_read and nw_ses_resume_read
     * can be used on the connections of a websocket server as on any
     * other */
    void (*on_drain)(nw_ses *ses);
    /* optional, with threads only
     *
     * called on every worker thread before it gets any connection, to set
     * up what that thread needs (timers, a nw_job, clients ...). svr is
     * the server of that thread, index is 0 .. thread_count - 1.
     * return < 0 if that fails: nw_ws_svr_create then fails */
    int (*on_thread_start)(nw_ws_svr *svr, uint32_t index);
    /* optional, with threads only
     *
     * called on every worker thread before it ends, after its connections
     * were closed */
    void (*on_thread_stop)(nw_ws_svr *svr, uint32_t index);
} nw_ws_svr_type;

/* the fields are for reading only */
struct nw_ws_svr {
    /* the nw_svr underneath: raw_svr->clt_count, nw_svr_next ... note that
     * it also holds the connections that have not finished the handshake.
     * with threads: the connections of this thread; in the server returned
     * by nw_ws_svr_create, the one that accepts, which has none */
    nw_svr *raw_svr;
    nw_ws_svr_type type;
    /* the privdata given to nw_ws_svr_create */
    void *privdata;
    uint32_t max_message_size;
    double keep_alive_timeout;
    double ping_interval;
    char *protocol;
    char *origin;
    nw_cache *conn_cache;
    /* with threads: the hub, NULL otherwise */
    nw_hub *hub;
    /* in the server of a worker thread: the one nw_ws_svr_create returned */
    struct nw_ws_svr *front;
    /* internal, only valid during nw_ws_svr_create */
    nw_ws_svr_cfg *creating_cfg;
};

/* create a server. cfg and type are copied. with threads the worker
 * threads are started here and the calling thread becomes the one that
 * accepts: it calls the next three functions and runs nw_loop_run.
 * return NULL if on_message is missing, on_privdata_alloc is set without
 * on_privdata_free, a bind is not SOCK_STREAM, or the nw_svr (or nw_hub)
 * underneath can't be created */
nw_ws_svr *nw_ws_svr_create(nw_ws_svr_cfg *cfg, nw_ws_svr_type *type, void *privdata);

/* bind and start accepting. return 0 or -1, see nw_svr_start */
int nw_ws_svr_start(nw_ws_svr *svr);

/* stop accepting new connections. existing ones are served on */
int nw_ws_svr_stop(nw_ws_svr *svr);

/* close every connection (on_close is called for each) and free the
 * server; with threads, also end the worker threads. don't call it from
 * a callback of this server */
void nw_ws_svr_release(nw_ws_svr *svr);

/* the server a session belongs to, for use in the callbacks */
nw_ws_svr *nw_ws_svr_of(nw_ses *ses);

/* what on_privdata_alloc returned for this connection, NULL if that
 * callback is not set. note: ses->privdata belongs to nw_ws */
void *nw_ws_privdata(nw_ses *ses);

/*
 * send a message to a connection.
 *
 * return 0 if it was sent or queued. return -1 if the connection has not
 * finished the handshake, is being closed, or sending failed (the send
 * buffer limit was reached, the socket broke): the connection is closed
 * then, don't use ses afterwards.
 */
int nw_ws_send_text(nw_ses *ses, const char *message);
int nw_ws_send_binary(nw_ses *ses, const void *data, size_t size);

/* the same for a connection known by its id (ses->id), for sending from
 * anywhere. return -1 as well if that connection is gone.
 * with threads, for a connection of another thread: the message is copied
 * and queued; 0 then means queued (if the connection is gone when its
 * thread gets to it, the message is dropped), -1 that it could not be */
int nw_ws_send_text_id(nw_ws_svr *svr, uint64_t id, const char *message);
int nw_ws_send_binary_id(nw_ws_svr *svr, uint64_t id, const void *data, size_t size);

/* send a message to every connection that finished the handshake. a
 * connection that can't take it is closed, the others still get it.
 * return the number of connections it was sent to.
 * with threads it goes to the connections of all threads, the frame being
 * built once; the return value is then 0, or -1 if some thread could not
 * be reached (the others still get it) */
int nw_ws_broadcast_text(nw_ws_svr *svr, const char *message);
int nw_ws_broadcast_binary(nw_ws_svr *svr, const void *data, size_t size);

/*
 * close a connection the websocket way: a close frame with code (one of
 * NW_WS_CLOSE_...) and an optional reason (NULL for none, at most 123
 * bytes are sent) goes out, then the connection is closed once the client
 * has answered or after a short wait. on_close is called when it is
 * actually closed. a connection that has not finished the handshake is
 * closed at once.
 */
void nw_ws_close(nw_ses *ses, uint16_t code, const char *reason);

/* the same by id. nothing happens if the connection is gone. return 0, or
 * -1 if, with threads, the request could not be queued */
int nw_ws_close_id(nw_ws_svr *svr, uint64_t id, uint16_t code, const char *reason);

# endif
