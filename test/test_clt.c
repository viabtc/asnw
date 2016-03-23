/*
 * Description: 
 *     History: yang@haipo.me, 2016/03/22, create
 */

# include <stdio.h>
# include <errno.h>
# include <stdlib.h>
# include <unistd.h>
# include <errno.h>
# include <error.h>

# include "nw_clt.h"
# include "nw_timer.h"

int decode_pkg(nw_ses *ses, void *data, size_t max)
{
    char *s = data;
    for (size_t i = 0; i < max; ++i) {
        if (s[i] == '\n')
            return i + 1;
    }
    return 0;
}

void on_connect(nw_ses *ses, bool result)
{
    if (result) {
        printf("connect to: %s success\n", nw_sock_human_addr(&ses->peer_addr));
    } else {
        printf("connect to: %s fail: %s\n", nw_sock_human_addr(&ses->peer_addr), strerror(errno));
    }
}

void on_close(nw_ses *ses)
{
    printf("connection to: %s close\n", nw_sock_human_addr(&ses->peer_addr));
}

void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    ((char *)data)[size] = 0;
    printf("from: %s recv: %zu: %s", nw_sock_human_addr(&ses->peer_addr), size, (char *)data);
}

void on_error_msg(nw_ses *ses, const char *msg)
{
    printf("error occur: %s, perr: %s\n", msg, nw_sock_human_addr(&ses->peer_addr));
}

void on_timeout(nw_timer *timer, void *privdata)
{
    nw_clt *clt = privdata;
    if (nw_clt_connected(clt)) {
        char *msg = "ping\n";
        nw_ses_send(&clt->ses, msg, strlen(msg));
    }
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        printf("usage: %s addr\n", argv[0]);
        exit(0);
    }

    nw_clt_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_pkg_size = 10240;
    if (nw_sock_cfg_parse(argv[1], &cfg.addr, &cfg.sock_type) < 0) {
        error(1, errno, "parse peer addr: %s fail", argv[1]);
    }

    nw_clt_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg   = decode_pkg;
    type.on_connect   = on_connect;
    type.on_close     = on_close;
    type.on_recv_pkg  = on_recv_pkg;
    type.on_error_msg = on_error_msg;

    nw_clt *clt = nw_clt_create(&cfg, &type, NULL);
    if (clt == NULL) {
        error(1, errno, "nw_clt_create fail");
    }
    nw_clt_start(clt);

    nw_timer timer;
    nw_timer_set(&timer, 1.0, true, on_timeout, clt);
    nw_timer_start(&timer);

    nw_loop_run();

    return 0;
}

