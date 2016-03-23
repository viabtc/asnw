/*
 * Description: 
 *     History: yang@haipo.me, 2016/03/20, create
 */

# include <stdio.h>

# include "nw_timer.h"

void on_timer(nw_timer *timer, void *private)
{
    int *count = (int *)private;
    *count += 1;
    printf("repeat %d\n", *count);
    printf("remaining %f\n", nw_timer_remaining(timer));
    if (*count == 10)
    {
        nw_timer_stop(timer);
    }
}

int main()
{
    int count = 0;
    nw_timer timer;
    nw_timer_set(&timer, 1.0, true, on_timer, &count);
    nw_timer_start(&timer);

    nw_loop_run();
    printf("stop!\n");

    return 0;
}

