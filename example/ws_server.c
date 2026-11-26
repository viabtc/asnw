/*
 * Description: example: websocket server, a chat room
 *     History: 2026/11/25, create
 */

# include <stdio.h>
# include <stdlib.h>
# include <string.h>

# include "nw_ws.h"

/*
 * every text message is sent to everybody who is connected.
 *
 * try it with:
 *   ./ws_server 'tcp@127.0.0.1:8081'
 * and in the console of two browser tabs:
 *   ws = new WebSocket("ws://127.0.0.1:8081/room?name=alice");
 *   ws.onmessage = e => console.log(e.data);
 *   ws.send("hello");
 *
 * with a number in front of the addresses it runs on that many threads:
 *   ./ws_server 4 'tcp@127.0.0.1:8081'
 * the code is the same either way: it keeps nothing but per connection
 * data, and a broadcast reaches the connections of every thread.
 */

struct user {
    char name[32];
};

static void *on_privdata_alloc(void *svr)
{
    return calloc(1, sizeof(struct user));
}

static void on_privdata_free(void *svr, void *privdata)
{
    free(privdata);
}

/* the request is only valid here: take the name out of the query string
 * and keep it in the connection's own data */
static int on_upgrade(nw_ses *ses, nw_http_request *req)
{
    struct user *user = nw_ws_privdata(ses);
    const char *name = strstr(req->query, "name=");
    snprintf(user->name, sizeof(user->name), "%s", name ? name + 5 : "anonymous");
    user->name[strcspn(user->name, "&")] = '\0';
    if (user->name[0] == '\0') {
        return -1;
    }
    printf("upgrade: %s %s from %s as %s\n", req->method, req->path, nw_sock_human_addr(&ses->peer_addr), user->name);
    return 0;
}

static void on_open(nw_ses *ses)
{
    struct user *user = nw_ws_privdata(ses);
    char text[128];
    snprintf(text, sizeof(text), "* %s joined", user->name);
    nw_ws_broadcast_text(nw_ws_svr_of(ses), text);
}

static void on_message(nw_ses *ses, bool binary, void *data, size_t size)
{
    struct user *user = nw_ws_privdata(ses);
    if (binary) {
        nw_ws_close(ses, NW_WS_CLOSE_UNSUPPORTED, "text only");
        return;
    }
    char text[1024];
    snprintf(text, sizeof(text), "%s: %s", user->name, (char *)data);
    int count = nw_ws_broadcast_text(nw_ws_svr_of(ses), text);
    printf("%s (to %d)\n", text, count);
}

/* the connection that is closing is already out of the server, so the
 * broadcast reaches everybody else */
static void on_close(nw_ses *ses)
{
    struct user *user = nw_ws_privdata(ses);
    char text[128];
    snprintf(text, sizeof(text), "* %s left", user->name);
    nw_ws_broadcast_text(nw_ws_svr_of(ses), text);
    printf("close: %s\n", user->name);
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    printf("error: %s, peer: %s\n", msg, nw_sock_human_addr(&ses->peer_addr));
}

int main(int argc, char *argv[])
{
    uint32_t thread_count = 0;
    if (argc >= 2 && strspn(argv[1], "0123456789") == strlen(argv[1])) {
        thread_count = atoi(argv[1]);
        argc--;
        argv++;
    }
    if (argc < 2) {
        printf("usage: %s [threads] bind...\n", argv[0]);
        exit(0);
    }

    int bind_count = argc - 1;
    nw_svr_bind *binds = calloc(bind_count, sizeof(nw_svr_bind));
    if (binds == NULL) {
        printf("malloc fail\n");
        exit(1);
    }
    for (int i = 0; i < bind_count; ++i) {
        int ret = nw_sock_addr_parse(argv[i + 1], &binds[i].addr, &binds[i].sock_type);
        if (ret < 0) {
            printf("parse bind: %s fail: %d\n", argv[i + 1], ret);
            exit(1);
        }
    }

    nw_ws_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.thread_count = thread_count;
    cfg.bind_count = bind_count;
    cfg.binds = binds;
    cfg.buf_limit = 16;
    cfg.idle_timeout = 60;
    cfg.ping_interval = 20;

    nw_ws_svr_type type;
    memset(&type, 0, sizeof(type));
    type.on_upgrade = on_upgrade;
    type.on_open = on_open;
    type.on_message = on_message;
    type.on_close = on_close;
    type.on_error_msg = on_error_msg;
    type.on_privdata_alloc = on_privdata_alloc;
    type.on_privdata_free = on_privdata_free;

    nw_ws_svr *svr = nw_ws_svr_create(&cfg, &type, NULL);
    if (svr == NULL) {
        printf("nw_ws_svr_create fail\n");
        exit(1);
    }
    if (nw_ws_svr_start(svr) < 0) {
        printf("nw_ws_svr_start fail\n");
        exit(1);
    }

    printf("websocket server start\n");
    nw_loop_run();
    printf("websocket server stop\n");

    return 0;
}
