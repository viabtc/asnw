/*
 * Description: example: a chat server on several threads, see nw_hub.h
 *     History: yang@haipo.me, 2026/11/25, create
 *
 *   hub_server 4 'tcp@127.0.0.1:1234'
 *   nc 127.0.0.1 1234          (in a few terminals)
 *
 * every line a client sends goes to all clients, whichever thread they
 * are on. "who" tells a client where it is, "quit" says goodbye.
 */

# include <stdio.h>
# include <stdlib.h>
# include <string.h>

# include "nw_hub.h"

static nw_hub *hub;

/* one per worker thread, no lock needed: only that thread touches it */
static __thread uint64_t lines_handled;

static int decode_pkg(nw_ses *ses, void *data, size_t max)
{
    char *s = data;
    for (size_t i = 0; i < max; ++i) {
        if (s[i] == '\n')
            return i + 1;
    }
    return 0;
}

static void on_new_connection(nw_ses *ses)
{
    printf("thread %d: new connection %s\n", nw_hub_index(hub), nw_sock_human_addr(&ses->peer_addr));
}

static void on_connection_close(nw_ses *ses)
{
    printf("thread %d: connection %s closed\n", nw_hub_index(hub), nw_sock_human_addr(&ses->peer_addr));
}

/* runs on the thread of this connection, at the same time as the same
 * function for connections of the other threads */
static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    char line[1200];
    int len;
    lines_handled++;

    if (size >= 3 && memcmp(data, "who", 3) == 0) {
        /* ses->svr is the nw_svr of this thread */
        nw_svr *svr = ses->svr;
        len = snprintf(line, sizeof(line), "you are %llu on thread %d, which has %u connections and handled %llu lines\n",
                (unsigned long long)ses->id, nw_hub_index(hub), svr->clt_count, (unsigned long long)lines_handled);
        nw_ses_send(ses, line, len);
    } else if (size >= 4 && memcmp(data, "quit", 4) == 0) {
        nw_ses_send(ses, "bye\n", 4);
        nw_svr_shutdown_clt(ses->svr, ses);
    } else {
        /* to everybody on every thread */
        len = snprintf(line, sizeof(line), "[%llu] %.*s", (unsigned long long)ses->id, (int)size, (char *)data);
        nw_hub_broadcast(hub, line, len);
    }
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    printf("error: %s, peer: %s\n", msg, nw_sock_human_addr(&ses->peer_addr));
}

static int on_thread_start(nw_hub *h, uint32_t index)
{
    printf("thread %u started\n", index);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        printf("usage: %s threads bind...\n", argv[0]);
        exit(0);
    }

    int bind_count = argc - 2;
    nw_svr_bind *bind_arr = calloc(bind_count, sizeof(nw_svr_bind));
    for (int i = 0; i < bind_count; ++i) {
        if (nw_sock_cfg_parse(argv[i + 2], &bind_arr[i].addr, &bind_arr[i].sock_type) < 0) {
            printf("parse bind: %s fail\n", argv[i + 2]);
            exit(0);
        }
    }

    nw_hub_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.svr.bind_count = bind_count;
    cfg.svr.bind_arr = bind_arr;
    cfg.svr.max_pkg_size = 1024;
    cfg.svr.buf_limit = 1024;
    cfg.thread_count = atoi(argv[1]);
    cfg.queue_limit = 64 * 1024 * 1024;

    nw_hub_type type;
    memset(&type, 0, sizeof(type));
    type.svr.decode_pkg = decode_pkg;
    type.svr.on_new_connection = on_new_connection;
    type.svr.on_connection_close = on_connection_close;
    type.svr.on_recv_pkg = on_recv_pkg;
    type.svr.on_error_msg = on_error_msg;
    type.on_thread_start = on_thread_start;

    hub = nw_hub_create(&cfg, &type, NULL);
    if (hub == NULL) {
        printf("nw_hub_create fail\n");
        exit(0);
    }
    if (nw_hub_start(hub) < 0) {
        printf("nw_hub_start fail\n");
        exit(0);
    }
    free(bind_arr);

    printf("hub server start, %u threads\n", hub->thread_count);
    nw_loop_run();
    printf("hub server stop\n");

    nw_hub_release(hub);
    return 0;
}
