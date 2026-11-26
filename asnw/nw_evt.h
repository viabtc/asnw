/*
 * Description: the event loop, a thin layer over libev
 *     History: yang@haipo.me, 2016/03/19, create
 */

# ifndef _NW_EVT_H_
# define _NW_EVT_H_

# include "ev.h"

/*
 * asnw runs on one libev loop per thread. everything created on a thread -
 * servers, clients, timers, states, job results - is driven by the loop of
 * that thread and runs on it, inside nw_loop_run.
 *
 * a program that never starts a thread has one loop and need not care.
 *
 * several threads can each run a loop of their own, side by side: nothing
 * in the library is shared between them. but an object belongs to the
 * thread that created it, and so do its connections: only that thread may
 * call a function on them (send, close, release ...), and its callbacks
 * run there. no function may be called on another thread's object, there
 * is no locking. to hand something to another thread, use a queue of your
 * own with an ev_async watcher on the loop of the receiver. the one
 * exception is the on_job callback of nw_job, see there.
 */

/* the loop of the calling thread, valid after nw_loop_init. use it to add
 * your own libev watchers to the same loop */
extern __thread struct ev_loop *nw_default_loop;

/* create the loop of the calling thread. calling it again is a no-op.
 * nw_svr_create, nw_clt_create, nw_timer_set, nw_state_create and
 * nw_job_create call it for you */
void nw_loop_init(void);

/* destroy the loop of the calling thread. a thread other than the main one
 * calls this before it exits, after it has released everything it created,
 * or the loop is leaked. afterwards the thread can start over */
void nw_loop_release(void);

/* run the loop. returns after nw_loop_break, or when nothing is left to
 * wait for (no listening server, connection or active timer) */
void nw_loop_run(void);

/* make nw_loop_run return once the current callback is done. like the
 * other functions here it acts on the loop of the calling thread: to stop
 * another thread, tell that thread */
void nw_loop_break(void);

/* internal: have fn called when the loop of the calling thread is
 * released, before it is destroyed. for what a module keeps per thread
 * (nw_dns: its resolver threads). return 0, or -1 if there is no room */
int nw_loop_at_release(void (*fn)(void));

/* make nw_loop_run return after waittime seconds, e.g. to let pending data
 * go out before exit. only has an effect while the loop is running, i.e.
 * when called from a callback */
void nw_loop_break_later(double waittime);

# endif
