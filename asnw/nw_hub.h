/*
 * Description: a server that spreads its connections over several threads
 *     History: yang@haipo.me, 2026/11/25, create
 */

# ifndef _NW_HUB_H_
# define _NW_HUB_H_

# include <stdint.h>
# include <pthread.h>

# include "nw_svr.h"

/*
 * nw_hub is a nw_svr on several threads. one thread, the one that creates
 * the hub, accepts the connections and deals them out in turn to a number
 * of worker threads. each worker has an event loop and a nw_svr of its
 * own, and a connection stays with the worker it was given to for all its
 * life.
 *
 *   hub thread     accepts, runs nw_loop_run like any asnw program
 *   worker 0       nw_svr: connections 0, n, 2n ...
 *   worker 1       nw_svr: connections 1, n + 1 ...
 *   ...
 *
 * you write the same callbacks as for a nw_svr. they are called on the
 * worker thread of the connection, and inside them everything works as in
 * a single threaded server: ses->svr is the nw_svr of that worker (the
 * privdata of every nw_svr of a hub is the one given to nw_hub_create),
 * and every nw_svr function
 * can be used on it and on the connections of the same worker
 * (nw_ses_send, nw_svr_close, nw_svr_shutdown, the timeouts, the
 * iterator ...). there are no locks to take.
 *
 * what is new is that connections on different workers cannot touch each
 * other directly, and that your own data, if several connections share
 * it, is now reached from several threads at once: guard it yourself, or
 * keep it per thread (see on_thread_start).
 *
 * to reach a connection from another thread - another worker, the hub
 * thread, a thread of your own - there is its id and the functions below:
 * nw_hub_send, nw_hub_close, nw_hub_shutdown, nw_hub_broadcast. they
 * may be called from any thread. on the worker that owns the connection
 * they act at once; from elsewhere the request is queued for that worker
 * (the data is copied) and carried out a moment later.
 *
 * for anything else that has to happen on a certain worker there is
 * nw_hub_run, which runs a function of yours there.
 *
 * ids are unique in the whole hub, never 0 and never reused; the worker
 * of a connection can be told from its id.
 *
 * only connection oriented binds are possible: stream (tcp, unix) and
 * seqpacket. see example/hub_server.c.
 */

typedef struct nw_hub nw_hub;

/* a function to run on a worker thread, see nw_hub_run. data is only
 * valid during the call */
typedef void (*nw_hub_callback)(nw_hub *hub, void *data, size_t size);

typedef struct nw_hub_cfg {
    /* addresses, sizes, limits and the idle timeout: see nw_svr_cfg. a
     * bind must not be of type SOCK_DGRAM */
    nw_svr_cfg svr;
    /* number of worker threads, 1 to NW_HUB_MAX_THREADS */
    uint32_t thread_count;
    /* max bytes of data (nw_hub_send, nw_hub_broadcast, nw_hub_run from
     * other threads) that may wait for one worker; beyond it they fail. a worker
     * that is stuck in a callback can then not pile up memory without
     * end. 0 means no limit */
    size_t queue_limit;
} nw_hub_cfg;

# define NW_HUB_MAX_THREADS 1024

typedef struct nw_hub_type {
    /* the callbacks of the connections, see nw_svr_type. each is called on
     * the worker thread of its connection: different connections run them
     * at the same time. the svr argument of on_privdata_alloc /
     * on_privdata_free is the nw_svr of the worker.
     *
     * two exceptions: on_accept is not used, leave it NULL. and
     * on_error_msg is also called on the hub thread, with the listening
     * session, when accepting fails. */
    nw_svr_type svr;
    /* optional
     *
     * called on every worker thread before it gets any connection, index
     * is 0 .. thread_count - 1. the place to set up what that thread
     * needs: its own timers, states, clients, a database connection.
     * everything created here runs on the loop of this thread.
     * return < 0 if that fails: nw_hub_create then fails as a whole */
    int (*on_thread_start)(nw_hub *hub, uint32_t index);
    /* optional
     *
     * called on every worker thread before it ends, after its connections
     * were closed. release here what on_thread_start created */
    void (*on_thread_stop)(nw_hub *hub, uint32_t index);
    /* optional, for a layer that wraps a nw_svr (nw_ws does). if set,
     * on_svr_release must be set too
     *
     * called on every worker thread, before on_thread_start, to create
     * the nw_svr of that worker in place of the default, which is
     * nw_svr_create(&cfg.svr, &type.svr, privdata). the server must have
     * the binds of cfg.svr and must not be started. its callbacks are the
     * ones used for the connections; type.svr then only serves the hub
     * thread (on_error_msg). return NULL on failure */
    nw_svr *(*on_svr_create)(nw_hub *hub, uint32_t index);
    /* called on the worker thread when it ends, after on_thread_stop, to
     * release what on_svr_create made. the connections are closed already */
    void (*on_svr_release)(nw_hub *hub, uint32_t index, nw_svr *svr);
} nw_hub_type;

/* the fields are for reading only */
struct nw_hub {
    uint32_t thread_count;
    struct nw_hub_worker *workers;
    /* accepts, on the hub thread. its binds holds the listening
     * sessions */
    nw_svr *acceptor;
    nw_hub_type type;
    nw_svr_cfg svr_cfg;
    size_t queue_limit;
    /* next worker to get a connection */
    uint32_t turn;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    /* the privdata given to nw_hub_create. reach it from a callback with
     * ((nw_svr *)ses->svr)->privdata, or nw_hub_of(ses)->privdata */
    void *privdata;
    /* the hubs created by one thread */
    struct nw_hub *next;
};

/* create a hub and start its worker threads (on_thread_start is called on
 * each). cfg and type are copied. nothing is bound yet. the calling thread
 * becomes the hub thread: it must call the next three functions, and run
 * nw_loop_run for connections to be accepted.
 * return NULL if cfg or type is incomplete or not possible, a thread or
 * socket can't be created, an on_thread_start failed or memory runs out */
nw_hub *nw_hub_create(nw_hub_cfg *cfg, nw_hub_type *type, void *privdata);

/* bind every address and start accepting. return 0, or -1 with errno set
 * if an address can't be bound */
int nw_hub_start(nw_hub *hub);

/* stop accepting. existing connections keep working. can't be started
 * again */
int nw_hub_stop(nw_hub *hub);

/* close every connection (on_close is called for each, on its
 * worker), end the worker threads (on_thread_stop) and free the hub.
 * returns when all of that is done. no other thread may still be using
 * the hub. don't call it from a callback of the hub */
void nw_hub_release(nw_hub *hub);

/*
 * the next four may be called from any thread.
 */

/* send to a connection by id.
 * on the worker of that connection: sent at once. return 0, or -1 if the
 * connection is gone or nw_ses_send failed.
 * from any other thread: data is copied and queued for that worker. return
 * 0 if queued, -1 if not (no such worker, queue_limit reached, out of
 * memory). 0 then does not say the connection still exists: if it is gone
 * by the time the worker gets to it, the data is dropped silently */
int nw_hub_send(nw_hub *hub, uint64_t id, const void *data, size_t size);

/* close a connection by id, see nw_svr_close. nothing happens if it is
 * gone. return 0, or -1 if the request could not be queued */
int nw_hub_close(nw_hub *hub, uint64_t id);

/* close a connection by id in an orderly way, see nw_svr_shutdown:
 * what was sent to it before, through this hub from the same thread,
 * still arrives. return as nw_hub_close */
int nw_hub_shutdown(nw_hub *hub, uint64_t id);

/* send to every connection of the hub. the data is copied once, whatever
 * the number of threads. connections that fail are closed like after any
 * failed send. return 0, or -1 if some worker could not be reached
 * (queue_limit, out of memory); the others still get it */
int nw_hub_broadcast(nw_hub *hub, const void *data, size_t size);

/* run callback on worker `index`, with a copy of data (size may be 0).
 * if the calling thread is that worker, it is called at once, before this
 * returns; otherwise a moment later. on the worker nw_hub_svr gives its
 * nw_svr. requests made by one thread to one worker are carried out in
 * the order they were made, this one and the four above alike.
 * return 0, or -1 if it could not be queued (no such worker, queue_limit,
 * out of memory) */
int nw_hub_run(nw_hub *hub, uint32_t index, nw_hub_callback callback, const void *data, size_t size);

/* run callback on every worker, each with the same copy of data, which
 * the callback must not change. return 0, or -1 if some worker could not
 * be reached; the others still run it */
int nw_hub_run_all(nw_hub *hub, nw_hub_callback callback, const void *data, size_t size);

/*
 * for the callbacks
 */

/* the hub a session belongs to: a connection, asked on its worker thread,
 * or a listening session, asked on the hub thread (in on_error_msg).
 * NULL for anything else */
nw_hub *nw_hub_of(nw_ses *ses);

/* the index of the calling worker thread of this hub, -1 if the calling
 * thread is not one of its workers */
int nw_hub_index(nw_hub *hub);

/* the nw_svr of the calling worker thread, which holds the connections of
 * this thread: for ses_count, the iterator, nw_svr_get. NULL if the
 * calling thread is not a worker of this hub */
nw_svr *nw_hub_svr(nw_hub *hub);

/* the index of the worker a connection id belongs to */
uint32_t nw_hub_index_of(uint64_t id);

# endif
