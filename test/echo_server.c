/*
 * Description: 
 *     History: yang@haipo.me, 2016/03/21, create
 */

# include <stdio.h>
# include <errno.h>
# include <stdlib.h>
# include <unistd.h>

# include "nw_svr.h"
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

void on_new_connection(nw_ses *ses)
{
    printf("new connection from: %s\n", nw_sock_human_addr(&ses->peer_addr));
}

void on_connection_close(nw_ses *ses)
{
    printf("connection: %s close\n", nw_sock_human_addr(&ses->peer_addr));
}

void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    char *str = malloc(size + 1);
    memcpy(str, data, size);
    str[size] = 0;
    printf("from: %s recv: %zu: %s", nw_sock_human_addr(&ses->peer_addr), size, str);
    if (nw_ses_send(ses, data, size) < 0) {
        printf("nw_ses_send fail\n");
    }
    free(str);
}

void on_recv_fd(nw_ses *ses, int fd)
{
    printf("recv fd: %d\n", fd);
    char buf[10240];
    int ret = read(fd, buf, sizeof(buf));
    if (ret < 0) {
        printf("read error: %s\n", strerror(errno));
    } else if (nw_ses_send(ses, buf, ret) < 0) {
        printf("nw_ses_send fail\n");
    }
    close(fd);
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
    if (argc < 2) {
        printf("usage: %s bind...\n", argv[0]);
        exit(0);
    }

    int bind_count = argc - 1;
    nw_svr_bind *bind_arr = malloc(sizeof(nw_svr_bind) * bind_count);
    if (bind_arr == NULL) {
        printf("malloc fail\n");
        exit(0);
    }
    memset(bind_arr, sizeof(nw_svr_bind) * bind_count, 0);
    for (int i = 0; i < bind_count; ++i) {
        int ret = nw_sock_cfg_parse(argv[i + 1], &bind_arr[i].addr, &bind_arr[i].sock_type);
        if (ret < 0) {
            printf("parse bind: %s fail: %d\n", argv[1], ret);
            exit(0);
        }
    }

    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = bind_count;
    cfg.bind_arr = bind_arr;
    cfg.max_pkg_size = 10240;

    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = decode_pkg;
    type.on_new_connection = on_new_connection;
    type.on_connection_close = on_connection_close;
    type.on_recv_pkg = on_recv_pkg;
    type.on_recv_fd = on_recv_fd;
    type.on_error_msg = on_error_msg;

    nw_svr *svr = nw_svr_create(&cfg, &type, NULL);
    if (svr == NULL) {
        printf("nw_svr_create fail\n");
        exit(0);
    }
    nw_svr_start(svr);

    nw_timer timer;
    nw_timer_set(&timer, 5.0, true, on_timer, svr);
    nw_timer_start(&timer);

    printf("echo server start\n");
    nw_loop_run();
    printf("echo server stop\n");

    return 0;
}

