/*
 * Description: example: echo server on any number of addresses
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

void on_open(nw_ses *ses)
{
    printf("new connection from: %s\n", nw_sock_human_addr(&ses->peer_addr));
}

void on_close(nw_ses *ses)
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
    printf("error: %s, peer: %s\n", msg, nw_sock_human_addr(&ses->peer_addr));
}

void on_timer(nw_timer *timer, void *privdata)
{
    nw_svr *svr = (nw_svr *)privdata;
    printf("clt count: %u\n", svr->ses_count);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("usage: %s bind...\n", argv[0]);
        exit(0);
    }

    int bind_count = argc - 1;
    nw_svr_bind *binds = malloc(sizeof(nw_svr_bind) * bind_count);
    if (binds == NULL) {
        printf("malloc fail\n");
        exit(0);
    }
    memset(binds, 0, sizeof(nw_svr_bind) * bind_count);
    for (int i = 0; i < bind_count; ++i) {
        int ret = nw_sock_addr_parse(argv[i + 1], &binds[i].addr, &binds[i].sock_type);
        if (ret < 0) {
            printf("parse bind: %s fail: %d\n", argv[1], ret);
            exit(0);
        }
    }

    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = bind_count;
    cfg.binds = binds;
    cfg.max_pkg_size = 10240;

    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = decode_pkg;
    type.on_open = on_open;
    type.on_close = on_close;
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
