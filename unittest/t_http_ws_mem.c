/*
 * read_mem and write_mem in the cfg of nw_http and nw_ws reach the
 * sockets of the connections, with and without threads; left at 0 the
 * sockets keep what the system gives them.
 */
# include <sys/socket.h>
# include <netinet/in.h>
# include "common.h"
# include "nw_http.h"
# include "nw_ws.h"

/* the system doubles what is asked for, and reports the double */
# define READ_MEM  40000
# define WRITE_MEM 50000

static int seen, seen_read, seen_write;

static void look(nw_ses *ses)
{
    int read_mem = 0, write_mem = 0;
    CHECK_EQ(nw_sock_get_recv_buf(ses->sockfd, &read_mem), 0);
    CHECK_EQ(nw_sock_get_send_buf(ses->sockfd, &write_mem), 0);
    __sync_lock_test_and_set(&seen_read, read_mem);
    __sync_lock_test_and_set(&seen_write, write_mem);
    __sync_fetch_and_add(&seen, 1);
}

static void on_request(nw_ses *ses, nw_http_request *req)
{
    look(ses);
    nw_http_reply_body(ses, 200, "text/plain", "ok", 2);
}

static int on_upgrade(nw_ses *ses, nw_http_request *req)
{
    look(ses);
    return 0;
}

static void on_message(nw_ses *ses, bool binary, void *data, size_t size)
{
}

static int knock(nw_ses *listener, const char *request)
{
    nw_addr addr;
    CHECK_EQ(nw_sock_local_addr(listener->sockfd, &addr), 0);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    CHECK_EQ(connect(fd, (struct sockaddr *)&addr.in, sizeof(addr.in)), 0);
    CHECK_EQ(send(fd, request, strlen(request), MSG_NOSIGNAL), (ssize_t)strlen(request));
    __sync_lock_test_and_set(&seen, 0);
    for (int i = 0; i < 2000 && !__sync_fetch_and_add(&seen, 0); ++i)
        ut_run_for(0.005);
    CHECK_EQ(__sync_fetch_and_add(&seen, 0), 1);
    return fd;
}

static void check(bool set)
{
    int read_mem = __sync_fetch_and_add(&seen_read, 0);
    int write_mem = __sync_fetch_and_add(&seen_write, 0);
    if (set) {
        CHECK_EQ(read_mem, READ_MEM * 2);
        CHECK_EQ(write_mem, WRITE_MEM * 2);
    } else {
        CHECK(read_mem != READ_MEM * 2);
        CHECK(write_mem != WRITE_MEM * 2);
    }
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    static nw_svr_bind bind;
    CHECK_EQ(nw_sock_addr_parse("tcp@127.0.0.1:0", &bind.addr, &bind.sock_type), 0);

    for (int round = 0; round < 4; ++round) {
        uint32_t threads = round / 2 * 2;       /* 0, 0, 2, 2 */
        bool set = round % 2;

        nw_http_svr_cfg hc;
        memset(&hc, 0, sizeof(hc));
        hc.bind_count = 1;
        hc.binds = &bind;
        hc.thread_count = threads;
        hc.read_mem = set ? READ_MEM : 0;
        hc.write_mem = set ? WRITE_MEM : 0;
        nw_http_svr_type ht;
        memset(&ht, 0, sizeof(ht));
        ht.on_request = on_request;
        nw_http_svr *http = nw_http_svr_create(&hc, &ht, NULL);
        CHECK(http != NULL);
        CHECK_EQ(nw_http_svr_start(http), 0);
        int fd = knock(&http->raw_svr->binds[0], "GET / HTTP/1.1\r\nHost: test\r\n\r\n");
        check(set);
        close(fd);
        nw_http_svr_release(http);

        nw_ws_svr_cfg wc;
        memset(&wc, 0, sizeof(wc));
        wc.bind_count = 1;
        wc.binds = &bind;
        wc.thread_count = threads;
        wc.read_mem = set ? READ_MEM : 0;
        wc.write_mem = set ? WRITE_MEM : 0;
        nw_ws_svr_type wt;
        memset(&wt, 0, sizeof(wt));
        wt.on_upgrade = on_upgrade;
        wt.on_message = on_message;
        nw_ws_svr *ws = nw_ws_svr_create(&wc, &wt, NULL);
        CHECK(ws != NULL);
        CHECK_EQ(nw_ws_svr_start(ws), 0);
        fd = knock(&ws->raw_svr->binds[0],
                "GET /chat HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
        check(set);
        close(fd);
        nw_ws_svr_release(ws);
    }

    nw_loop_release();
    return 0;
}
