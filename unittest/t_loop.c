/*
 * nw_loop_break_later: the loop goes on for a while and then returns,
 * when asked from a callback; asked while the loop is not running it
 * does nothing.
 */
# include "common.h"

static double asked_at;
static int asked;

static void on_timer(nw_timer *timer, void *privdata)
{
    asked++;
    asked_at = ev_time();
    nw_loop_break_later(0.2);
    /* asked twice: the first one counts */
    nw_loop_break_later(30.0);
}

int main(void)
{
    nw_loop_init();

    /* not running: nothing is scheduled, the loop runs until it is told to stop */
    nw_loop_break_later(0.01);
    double begin = ev_time();
    ut_run_for(0.3);
    CHECK(ev_time() - begin >= 0.25);

    /* from a callback: the loop returns that much later, not at once */
    nw_timer timer;
    nw_timer_set(&timer, 0.05, false, on_timer, NULL);
    nw_timer_start(&timer);
    begin = ev_time();
    ut_run_for(10.0);
    double end = ev_time();
    CHECK_EQ(asked, 1);
    CHECK(end - asked_at >= 0.15);
    CHECK(end - begin < 5.0);

    /* and it is over with: the next run is not cut short */
    begin = ev_time();
    ut_run_for(0.3);
    CHECK(ev_time() - begin >= 0.25);

    nw_loop_release();
    return 0;
}
