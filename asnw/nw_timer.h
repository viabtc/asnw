/*
 * Description: timer, call a function after an interval, once or repeatedly
 *     History: yang@haipo.me, 2016/03/20, create
 */

# ifndef _NW_TIMER_H_
# define _NW_TIMER_H_

# include <stdbool.h>

# include "nw_loop.h"

/*
 * a nw_timer is owned by the caller (on the stack, in a struct ...) and must
 * stay valid, at the same address, for as long as it is started.
 */

struct nw_timer;
/* called on the loop thread. the callback may stop the timer, or stop, set
 * and start it again */
typedef void (*nw_timer_callback)(struct nw_timer *timer, void *privdata);

typedef struct nw_timer {
    ev_timer ev;
    struct ev_loop *loop;
    double interval;
    nw_timer_callback callback;
    void *privdata;
} nw_timer;

/*
 * prepare a timer: callback(timer, privdata) is called interval seconds
 * after nw_timer_start. with repeated, it is called every interval seconds
 * until nw_timer_stop; without, once.
 *
 * this only fills in the timer, nw_timer_start arms it. the timer must not
 * be started at this point: stop it before setting it again.
 */
void nw_timer_set(nw_timer *timer, double interval, bool repeated, nw_timer_callback callback, void *privdata);
/* arm the timer. no-op if it is already started */
void nw_timer_start(nw_timer *timer);
/* disarm the timer. no-op if it is not started. a one shot timer stops by
 * itself after it fired */
void nw_timer_stop(nw_timer *timer);
/* is the timer started and still going to fire? */
bool nw_timer_active(nw_timer *timer);
/* seconds until a started timer fires next */
double nw_timer_remaining(nw_timer *timer);

# endif
