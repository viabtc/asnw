/*
 * Description: 
 *     History: yang@haipo.me, 2016/03/30, create
 */

# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <assert.h>
# include <error.h>
# include <errno.h>

# include "ut_rpc_svr.h"
# include "ut_misc.h"

void on_new_connection(nw_ses *ses)
{
    printf("new connection from: %s\n", nw_sock_human_addr(&ses->peer_addr));
}

void on_connection_close(nw_ses *ses)
{
    printf("connection: %s close\n", nw_sock_human_addr(&ses->peer_addr));
}

void on_recv_pkg(nw_ses *ses, rpc_pkg *pkg)
{
    sds hex = hex_dump(pkg->body, pkg->body_size);
    printf("from: %s, cmd: %u\n%s\n", nw_sock_human_addr(&ses->peer_addr), pkg->command, hex);
    sdsfree(hex);
    rpc_send(ses, pkg);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("usage: %s bind...\n", argv[0]);
        exit(0);
    }

    int bind_count = argc - 1;
    nw_svr_bind *bind_arr = malloc(sizeof(nw_svr_bind) * bind_count);
    assert(bind_arr != NULL);
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

    rpc_svr_type type;
    memset(&type, 0, sizeof(type));
    type.on_recv_pkg = on_recv_pkg;
    type.on_new_connection = on_new_connection;
    type.on_connection_close = on_connection_close;

    rpc_svr *svr = rpc_svr_create(&cfg, &type);
    if (svr == NULL)
        error(1, errno, "rpc_svr_create fail");
    rpc_svr_start(svr);

    nw_loop_run();

    return 0;
}

