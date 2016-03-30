/*
 * Description: 
 *     History: yang@haipo.me, 2016/03/23, create
 */

# include <stdio.h>
# include <stdlib.h>
# include <stdint.h>
# include <string.h>
# include <errno.h>
# include <error.h>
# include <unistd.h>

# include "nw_svr.h"
# include "nw_clt.h"

int worker_id;
uint32_t max_pkg_size = 10240;
nw_svr *outer_svr;
nw_svr *inner_svr;
nw_clt *inner_clt;
const char *inner_sock_cfg = "seqpacket@/tmp/test_worker.sock";

int outer_decode_pkg(nw_ses *ses, void *data, size_t max)
{
    char *s = data;
    for (size_t i = 0; i < max; ++i) {
        if (s[i] == '\n')
            return i + 1;
    }
    return 0;
}

int outer_on_accept(nw_ses *ses, int sockfd, nw_addr_t *peer_addr)
{
    if (inner_svr->clt_count == 0) {
        printf("worker: %d, no available worker\n", worker_id);
        return -1;
    }
    int worker = rand() % inner_svr->clt_count;
    nw_ses *curr = inner_svr->clt_list_head;
    for (int i = 0; i < worker && curr != NULL; ++i) {
        curr = curr->next;
    }
    if (!curr) {
        return -1;
    }
    if (nw_ses_send_fd(curr, sockfd) < 0) {
        return -1;
    }

    // close it after success send
    close(sockfd);

    return 0;
}

void outer_on_new_connection(nw_ses *ses)
{
    printf("worker: %d: new connection from: %s\n", worker_id, nw_sock_human_addr(&ses->peer_addr));
}

void outer_on_connection_close(nw_ses *ses)
{
    printf("worker: %d: connection: %s close\n", worker_id, nw_sock_human_addr(&ses->peer_addr));
}

void outer_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    char *str = malloc(size + 1);
    memcpy(str, data, size);
    str[size] = 0;
    printf("worker: %d, from: %s recv: %zu: %s", worker_id, nw_sock_human_addr(&ses->peer_addr), size, str);
    if (nw_ses_send(ses, data, size) < 0) {
        printf("nw_ses_send fail\n");
    }
    free(str);
}

void outer_on_error_msg(nw_ses *ses, const char *msg)
{
    printf("worker: %d, outer error: %s\n", worker_id, msg);
}

void inner_svr_on_new_connection(nw_ses *ses)
{
    printf("new worker connection\n");
}

void inner_svr_on_connection_close(nw_ses *ses)
{
    printf("worker connection close\n");
}

void inner_svr_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    return;
}

void inner_svr_on_error_msg(nw_ses *ses, const char *msg)
{
    printf("listener error: %s\n", msg);
}

void inner_clt_on_connect(nw_ses *ses, bool result)
{
    if (result) {
        printf("worker: %d connect listener success\n", worker_id);
    } else {
        printf("worker: %d connect listener fail\n", worker_id);
    }
}

void inner_clt_on_close(nw_ses *ses)
{
    printf("worker: %d connection with listener close\n", worker_id);
}

void inner_clt_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    return;
}

void inner_clt_on_recv_fd(nw_ses *ses, int fd)
{
    if (nw_svr_add_clt_fd(outer_svr, fd) < 0) {
        printf("worker: %d: nw_svr_add_clt_fd fail\n", worker_id);
    } else {
        printf("worker: %d recv fd success\n", worker_id);
    }
}

void inner_clt_on_error_msg(nw_ses *ses, const char *msg)
{
    printf("worker: %d: clt error msg: %s\n", worker_id, msg);
}

int init_outer_svr(nw_svr_bind *bind)
{
    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_arr = bind;
    cfg.bind_count = 1;
    cfg.max_pkg_size = max_pkg_size;

    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = outer_decode_pkg;
    type.on_accept = outer_on_accept;
    type.on_new_connection = outer_on_new_connection;
    type.on_connection_close = outer_on_connection_close;
    type.on_recv_pkg = outer_on_recv_pkg;
    type.on_error_msg = outer_on_error_msg;

    outer_svr = nw_svr_create(&cfg, &type, NULL);
    if (outer_svr == NULL) {
        return -1;
    }

    return 0;
}

int init_inner_svr(void)
{
    nw_svr_bind bind;
    if (nw_sock_cfg_parse(inner_sock_cfg, &bind.addr, &bind.sock_type) < 0) {
        return -1;
    }

    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_arr = &bind;
    cfg.bind_count = 1;
    cfg.max_pkg_size = max_pkg_size;

    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.on_new_connection = inner_svr_on_new_connection;
    type.on_connection_close = inner_svr_on_connection_close;
    type.on_recv_pkg = inner_svr_on_recv_pkg;
    type.on_error_msg = inner_svr_on_error_msg;

    inner_svr = nw_svr_create(&cfg, &type, NULL);
    if (inner_svr == NULL) {
        return -1;
    }

    return 0;
}

int init_inner_clt(void)
{
    nw_clt_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (nw_sock_cfg_parse(inner_sock_cfg, &cfg.addr, &cfg.sock_type) < 0) {
        return -1;
    }
    cfg.max_pkg_size = max_pkg_size;

    nw_clt_type type;
    memset(&type, 0, sizeof(type));
    type.on_connect = inner_clt_on_connect;
    type.on_close = inner_clt_on_close;
    type.on_recv_pkg = inner_clt_on_recv_pkg;
    type.on_recv_fd = inner_clt_on_recv_fd;
    type.on_error_msg = inner_clt_on_error_msg;

    inner_clt = nw_clt_create(&cfg, &type, NULL);
    if (inner_clt == NULL) {
        return -1;
    }

    return 0;
}

int init_as_listener(nw_svr_bind *bind)
{
    if (init_outer_svr(bind) < 0) {
        printf("init_outer_svr fail\n");
        return -1;
    }
    if (nw_svr_start(outer_svr) < 0) {
        printf("nw_svr_start fail\n");
        return -1;
    }
    if (init_inner_svr() < 0) {
        printf("init_inner_svr fail\n");
        return -1;
    }
    if (nw_svr_start(inner_svr) < 0) {
        printf("nw_svr_start fail\n");
        return -1;
    }

    printf("listener start\n");

    return 0;
}

int init_as_worker(nw_svr_bind *bind)
{
    if (init_outer_svr(bind) < 0) {
        printf("init_outer_svr fail\n");
        return -1;
    }
    if (init_inner_clt() < 0) {
        printf("init_inner_clt fail\n");
        return -1;
    }

    printf("worker: %d start\n", worker_id);

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        printf("usage: %s bind worker-num\n", argv[0]);
        return 0;
    }

    nw_svr_bind bind;
    if (nw_sock_cfg_parse(argv[1], &bind.addr, &bind.sock_type) < 0) {
        printf("parse bind: %s fail\n", argv[1]);
        return 0;
    }
    if (bind.sock_type != SOCK_STREAM) {
        printf("not stream: %s\n", argv[1]);
        return 0;
    }
    size_t worker_num = atoi(argv[2]);
    if (worker_num == 0) {
        printf("invalid num: %s\n", argv[2]);
    }

    for (size_t i = 0; i < worker_num; ++i) {
        int pid = fork();
        if (pid < 0) {
            error(1, errno, "fork error");
        } else if (pid == 0) {
            worker_id = i + 1;
            if (init_as_worker(&bind) < 0) {
                error(1, errno, "init worker: %d fail", worker_id);
            }
            break;
        }
    }

    if (worker_id == 0) {
        if (init_as_listener(&bind) < 0) {
            error(1, errno, "init listener fail");
        }
    } else {
        if (nw_clt_start(inner_clt) < 0) {
            error(1, errno, "nw_clt_start fail");
        }
    }

    daemon(true, true);
    nw_loop_run();

    return 0;
}

