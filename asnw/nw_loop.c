/*
 * Description: the event loop, a thin layer over libev
 *     History: yang@haipo.me, 2016/03/19, create
 */

# include <syslog.h>
# include <unistd.h>
# include <limits.h>
# include <libgen.h>
# include <pthread.h>

# include "nw_loop.h"

/* one loop per thread */
__thread struct ev_loop *nw_default_loop;
static __thread int initialized;
static __thread int running;
static __thread ev_timer break_timer;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void fatal_error(const char *msg)
{
    char exe_path[PATH_MAX] = "asnw";
    ssize_t ret = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (ret > 0)
        exe_path[ret] = 0;

    openlog(basename(exe_path), LOG_PERROR | LOG_PID, LOG_DAEMON);
    syslog(LOG_ERR, "%s", msg);
    closelog();
}

/* process wide */
static void init_once(void)
{
    ev_set_syserr_cb(fatal_error);
}

void nw_loop_init(void)
{
    if (initialized)
        return;

    unsigned int flag = 0;
    flag |= EVFLAG_AUTO;
    flag |= EVFLAG_NOENV;
    flag |= EVFLAG_FORKCHECK;
    pthread_once(&once, init_once);
    nw_default_loop = ev_loop_new(flag);
    initialized = 1;
}

/* see nw_loop_at_release */
# define NW_RELEASE_HOOK_MAX 8
static __thread void (*release_hooks[NW_RELEASE_HOOK_MAX])(void);
static __thread int release_hook_count;

int nw_loop_at_release(void (*fn)(void))
{
    nw_loop_init();
    if (release_hook_count == NW_RELEASE_HOOK_MAX)
        return -1;
    release_hooks[release_hook_count++] = fn;
    return 0;
}

void nw_loop_release(void)
{
    if (!initialized)
        return;
    if (running)
        return;

    /* the last one to come is the first to go */
    while (release_hook_count > 0)
        release_hooks[--release_hook_count]();

    if (ev_is_active(&break_timer))
        ev_timer_stop(nw_default_loop, &break_timer);
    ev_loop_destroy(nw_default_loop);
    nw_default_loop = NULL;
    initialized = 0;
}

void nw_loop_run(void)
{
    if (!initialized)
        return;

    running = 1;
    ev_run(nw_default_loop, 0);
    running = 0;
}

void nw_loop_break(void)
{
    if (!initialized)
        return;
    if (!running)
        return;

    /* a nw_loop_break_later still waiting is for this run, not the next */
    if (ev_is_active(&break_timer))
        ev_timer_stop(nw_default_loop, &break_timer);
    ev_break(nw_default_loop, EVBREAK_ALL);
    running = 0;
}

static void on_break_timer(struct ev_loop *loop, ev_timer *ev, int events)
{
    nw_loop_break();
}

void nw_loop_break_later(double waittime)
{
    if (!initialized)
        return;
    if (!running)
        return;

    if (!ev_is_active(&break_timer)) {
        ev_timer_init(&break_timer, on_break_timer, waittime, 0);
        ev_timer_start(nw_default_loop, &break_timer);
    }
}
