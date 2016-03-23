/*
 * Description: net workd event, base on libev
 *     History: yang@haipo.me, 2016/03/19, create
 */

# ifndef _NW_EVT_H_
# define _NW_EVT_H_

# include "ev.h"

extern struct ev_loop *nw_default_loop;

void nw_loop_init(void);
void nw_loop_run(void);
void nw_loop_break(void);

# endif

