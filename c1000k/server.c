/*
 * Description: 
 *     History: yang@haipo.me, 2016/03/30, create
 */

# include <stdio.h>
# include <stdlib.h>
# include <unistd.h>
# include <errno.h>
# include <error.h>

# include "nw_svr.h"
# include "nw_timer.h"
# include "ut_misc.h"

int decode_pkg(nw_ses *ses, void *data, size_t max)
{
    char *s = data;
    for (size_t i = 0; i < max; ++i) {
        if (s[i] == '\n')
            return i + 1;
    }
    return 0;
}

void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
}

void on_error_msg(nw_ses *ses, const char *msg)
{
    printf("error occur: %s, perr: %s\n", msg, nw_sock_human_addr(&ses->peer_addr));
}

void on_timer(nw_timer *timer, void *privdata)
{
    nw_svr *svr = (nw_svr *)privdata;
    printf("clt count: %u\n", svr->clt_count);
}

int main(int argc, char *argv[])
{
    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 100;
    cfg.bind_arr = malloc(sizeof(nw_svr_bind) * cfg.bind_count);
    cfg.max_pkg_size = 1024;

    for (int i = 0; i < cfg.bind_count; ++i) {
        char bind[1024];
        snprintf(bind, sizeof(bind), "tcp@127.0.0.1:%d", 5000 + i);
        nw_sock_cfg_parse(bind, &cfg.bind_arr[i].addr, &cfg.bind_arr[i].sock_type);
    }

    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = decode_pkg;
    type.on_recv_pkg = on_recv_pkg;
    type.on_error_msg = on_error_msg;

    nw_svr *svr = nw_svr_create(&cfg, &type, NULL);
    if (svr == NULL) {
        printf("nw_svr_create fail\n");
        exit(0);
    }
    nw_svr_start(svr);

    nw_timer timer;
    nw_timer_set(&timer, 1.0, true, on_timer, svr);
    nw_timer_start(&timer);

    set_file_limit(1001000);

    printf("server start\n");
    nw_loop_run();

    return 0;
}

