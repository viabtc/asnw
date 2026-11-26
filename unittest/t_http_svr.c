/*
 * nw_http_svr end to end, driven by plain sockets so that every byte and
 * every packet boundary is under control: keep-alive, bodies arriving in
 * pieces, chunked, HEAD, 100-continue, answering later by id, refusals,
 * timeouts and the ways a connection ends.
 */
# include "common.h"
# include "nw_http.h"

# define BIG_SIZE (2 * 1024 * 1024)

static nw_http_svr *svr;
static nw_addr tcp_addr;
static nw_addr unix_addr;
static char *big_body;
static int request_count;
static int refused_count;
static uint64_t async_ids[16];
static int async_count;
static nw_timer async_timer;
static int async_fail_count;

static void on_async_timer(nw_timer *timer, void *privdata)
{
    for (int i = 0; i < async_count; ++i) {
        nw_http_response rsp;
        memset(&rsp, 0, sizeof(rsp));
        rsp.status = 200;
        rsp.body = "later\n";
        rsp.body_size = 6;
        if (nw_http_reply_id(svr, async_ids[i], &rsp) < 0)
            async_fail_count++;
    }
    async_count = 0;
}

static void on_request(nw_ses *ses, nw_http_request *req)
{
    request_count++;
    CHECK(nw_http_svr_of(ses) == svr);

    if (strcmp(req->path, "/hello") == 0) {
        CHECK_EQ(nw_http_reply_body(ses, 200, "text/plain", "hello\n", 6), 0);
        /* a request is answered once */
        CHECK_EQ(nw_http_reply_body(ses, 200, "text/plain", "again\n", 6), -1);
    } else if (strcmp(req->path, "/echo") == 0) {
        char count[16];
        snprintf(count, sizeof(count), "%u", req->header_count);
        const char *type = nw_http_get_header(req, "content-type");
        nw_http_response rsp;
        memset(&rsp, 0, sizeof(rsp));
        rsp.status = 200;
        nw_http_add_header(&rsp, "X-Method", req->method);
        nw_http_add_header(&rsp, "X-Query", req->query);
        nw_http_add_header(&rsp, "X-Header-Count", count);
        if (type)
            nw_http_add_header(&rsp, "Content-Type", type);
        /* the body is sent straight from the read buffer */
        rsp.body = req->body;
        rsp.body_size = req->body_size;
        CHECK_EQ(nw_http_reply(ses, &rsp), 0);
    } else if (strcmp(req->path, "/async") == 0) {
        async_ids[async_count++] = ses->id;
        if (!nw_timer_active(&async_timer)) {
            nw_timer_set(&async_timer, 0.05, false, on_async_timer, NULL);
            nw_timer_start(&async_timer);
        }
    } else if (strcmp(req->path, "/never") == 0) {
        /* no answer */
    } else if (strcmp(req->path, "/big") == 0) {
        CHECK_EQ(nw_http_reply_body(ses, 200, "application/octet-stream", big_body, BIG_SIZE), 0);
    } else if (strcmp(req->path, "/close") == 0) {
        nw_http_response rsp;
        memset(&rsp, 0, sizeof(rsp));
        rsp.status = 200;
        rsp.close = true;
        rsp.body = "bye\n";
        rsp.body_size = 4;
        CHECK_EQ(nw_http_reply(ses, &rsp), 0);
    } else if (strcmp(req->path, "/204") == 0) {
        CHECK_EQ(nw_http_reply_body(ses, 204, NULL, NULL, 0), 0);
    } else if (strcmp(req->path, "/headers") == 0) {
        nw_http_response rsp;
        memset(&rsp, 0, sizeof(rsp));
        rsp.status = 200;
        /* a line break in a header is refused, nothing is sent */
        nw_http_add_header(&rsp, "X-Evil", "a\r\nSet-Cookie: stolen=1");
        CHECK_EQ(nw_http_reply(ses, &rsp), -1);
        rsp.header_count = 0;
        nw_http_add_header(&rsp, "X-Evil\r\nX", "a");
        CHECK_EQ(nw_http_reply(ses, &rsp), -1);
        /* headers of our own: Date replaces the automatic one, the framing
         * ones are ignored */
        rsp.header_count = 0;
        rsp.status = 299;
        rsp.reason = "Custom Reason";
        nw_http_add_header(&rsp, "Date", "yesterday");
        nw_http_add_header(&rsp, "Content-Length", "9999");
        nw_http_add_header(&rsp, "Transfer-Encoding", "chunked");
        nw_http_add_header(&rsp, "Connection", "close");
        nw_http_add_header(&rsp, "X-Custom", "1");
        rsp.body = "ok";
        rsp.body_size = 2;
        CHECK_EQ(nw_http_reply(ses, &rsp), 0);
    } else {
        CHECK_EQ(nw_http_reply_body(ses, 404, "text/plain", "no such page\n", 13), 0);
    }
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    if (strncmp(msg, "request refused", 15) == 0)
        refused_count++;
}


# include "http_client.h"

/* a request that is refused: the status, then the connection is closed */
static void expect_refused(const char *request, int status)
{
    struct response rsp;
    struct client *c = client_new(&tcp_addr);
    int before = refused_count;
    client_send(c, request, strlen(request));
    CHECK(client_read(c, &rsp, false, 3.0));
    if (rsp.status != status) {
        fprintf(stderr, "expected %d, got %d for: %.60s\n", status, rsp.status, request);
        exit(1);
    }
    CHECK(strstr(rsp.head, "Connection: close\r\n") != NULL);
    CHECK(client_closed(c, 3.0));
    CHECK_EQ(refused_count, before + 1);
    client_free(c);
}

static uint32_t rnd_state = 7;

static uint32_t rnd(void)
{
    rnd_state = rnd_state * 1103515245 + 12345;
    return rnd_state >> 8;
}

int main(void)
{
    struct response rsp;
    struct client *c;

    big_body = malloc(BIG_SIZE);
    for (int i = 0; i < BIG_SIZE; ++i)
        big_body[i] = 'a' + i % 23;

    nw_svr_bind binds[2];
    char unix_cfg[256];
    int type;
    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1:0", &binds[0].addr, &binds[0].sock_type), 0);
    ut_unix_cfg(unix_cfg, sizeof(unix_cfg), "stream", "http.sock");
    CHECK_EQ(nw_sock_cfg_parse(unix_cfg, &binds[1].addr, &binds[1].sock_type), 0);
    CHECK_EQ(nw_sock_cfg_parse(unix_cfg, &unix_addr, &type), 0);

    nw_http_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 2;
    cfg.bind_arr = binds;
    cfg.max_request_size = 4096;

    nw_http_svr_type st;
    memset(&st, 0, sizeof(st));
    CHECK(nw_http_svr_create(&cfg, &st, NULL) == NULL);         /* on_request is required */
    st.on_request = on_request;
    st.on_error_msg = on_error_msg;
    nw_svr_bind udp;
    CHECK_EQ(nw_sock_cfg_parse("udp@127.0.0.1:0", &udp.addr, &udp.sock_type), 0);
    nw_http_svr_cfg bad_cfg = cfg;
    bad_cfg.bind_count = 1;
    bad_cfg.bind_arr = &udp;
    CHECK(nw_http_svr_create(&bad_cfg, &st, NULL) == NULL);     /* stream only */

    svr = nw_http_svr_create(&cfg, &st, NULL);
    CHECK(svr != NULL);
    CHECK_EQ(nw_http_svr_start(svr), 0);
    CHECK_EQ(nw_sock_host_addr(svr->raw_svr->bind_list[0].sockfd, &tcp_addr), 0);

    /* a GET, and a second one on the same connection */
    c = client_new(&tcp_addr);
    client_puts(c, "GET /hello HTTP/1.1\r\nHost: test\r\n\r\n");
    CHECK(client_read(c, &rsp, false, 3.0));
    CHECK_EQ(rsp.status, 200);
    CHECK(strncmp(rsp.head, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(strstr(rsp.head, "\r\nContent-Type: text/plain\r\n") != NULL);
    CHECK(strstr(rsp.head, "\r\nContent-Length: 6\r\n") != NULL);
    CHECK(strstr(rsp.head, "\r\nConnection: keep-alive\r\n") != NULL);
    CHECK(strstr(rsp.head, "\r\nDate: ") != NULL && strstr(rsp.head, " GMT\r\n") != NULL);
    CHECK_EQ(rsp.body_size, 6);
    CHECK(strcmp(rsp.body, "hello\n") == 0);
    client_puts(c, "GET /nothing HTTP/1.1\r\nHost: test\r\n\r\n");
    CHECK(client_read(c, &rsp, false, 3.0));
    CHECK_EQ(rsp.status, 404);
    CHECK(strncmp(rsp.head, "HTTP/1.1 404 Not Found\r\n", 24) == 0);
    /* empty lines between requests are tolerated */
    client_puts(c, "\r\n\r\nGET /hello HTTP/1.1\r\nHost: test\r\n\r\n");
    CHECK(client_read(c, &rsp, false, 3.0));
    CHECK_EQ(rsp.status, 200);
    CHECK_EQ(svr->raw_svr->clt_count, 1);
    CHECK_EQ(request_count, 3);
    client_free(c);

    /* the same over the unix socket bind */
    CHECK_EQ(simple(&unix_addr, "GET /hello HTTP/1.1\r\nHost: test\r\n\r\n", &rsp), 200);

    /* POST with a body, query and headers handed back */
    CHECK_EQ(simple(&tcp_addr, "POST /echo?a=1&b=2 HTTP/1.1\r\nHost: test\r\nContent-Type: text/x-test\r\n"
                "Content-Length: 5\r\n\r\nhello", &rsp), 200);
    CHECK(strcmp(rsp.body, "hello") == 0);
    CHECK(strstr(rsp.head, "\r\nX-Method: POST\r\n") != NULL);
    CHECK(strstr(rsp.head, "\r\nX-Query: a=1&b=2\r\n") != NULL);
    CHECK(strstr(rsp.head, "\r\nX-Header-Count: 3\r\n") != NULL);
    CHECK(strstr(rsp.head, "\r\nContent-Type: text/x-test\r\n") != NULL);

    /* chunked request body */
    CHECK_EQ(simple(&tcp_addr, "POST /echo HTTP/1.1\r\nHost: test\r\nTransfer-Encoding: chunked\r\n\r\n"
                "6\r\nchunk \r\n4;x=y\r\nbody\r\n0\r\nTrailer: 1\r\n\r\n", &rsp), 200);
    CHECK_EQ(rsp.body_size, 10);
    CHECK(strcmp(rsp.body, "chunk body") == 0);

    /* requests arriving in pieces, cut at random places, several per
     * connection: the framing has to resume where it stopped */
    static const char *requests[] = {
        "POST /echo?q HTTP/1.1\r\nHost: test\r\nContent-Length: 26\r\nX-A: 1\r\n\r\nabcdefghijklmnopqrstuvwxyz",
        "POST /echo HTTP/1.1\r\nHost: test\r\nTransfer-Encoding: chunked\r\n\r\n"
            "a\r\nabcdefghij\r\n10\r\nklmnopqrstuvwxyz\r\n0\r\n\r\n",
        "GET /echo HTTP/1.1\r\nHost: test\r\nAccept: */*\r\n\r\n",
    };
    c = client_new(&tcp_addr);
    for (int iter = 0; iter < 60; ++iter) {
        const char *request = requests[iter % 3];
        size_t size = strlen(request);
        size_t sent = 0;
        while (sent < size) {
            size_t piece = iter < 3 ? 1 : 1 + rnd() % 40;      /* the first rounds byte by byte */
            if (piece > size - sent)
                piece = size - sent;
            client_send(c, request + sent, piece);
            sent += piece;
            ut_run_for(0.001);
        }
        CHECK(client_read(c, &rsp, false, 3.0));
        CHECK_EQ(rsp.status, 200);
        if (iter % 3 == 2)
            CHECK_EQ(rsp.body_size, 0);
        else
            CHECK(strcmp(rsp.body, "abcdefghijklmnopqrstuvwxyz") == 0);
    }
    client_free(c);

    /* HEAD: the head of the GET, no body, and the connection stays usable */
    c = client_new(&tcp_addr);
    client_puts(c, "HEAD /hello HTTP/1.1\r\nHost: test\r\n\r\n");
    CHECK(client_read(c, &rsp, true, 3.0));
    CHECK_EQ(rsp.status, 200);
    CHECK(strstr(rsp.head, "\r\nContent-Length: 6\r\n") != NULL);
    client_puts(c, "GET /hello HTTP/1.1\r\nHost: test\r\n\r\n");
    CHECK(client_read(c, &rsp, false, 3.0));
    CHECK(strcmp(rsp.body, "hello\n") == 0);            /* no stray body in between */
    client_free(c);

    /* 204 has no Content-Length */
    CHECK_EQ(simple(&tcp_addr, "GET /204 HTTP/1.1\r\nHost: test\r\n\r\n", &rsp), 204);
    CHECK(strcasestr(rsp.head, "Content-Length") == NULL);

    /* header checks on the way out */
    CHECK_EQ(simple(&tcp_addr, "GET /headers HTTP/1.1\r\nHost: test\r\n\r\n", &rsp), 299);
    CHECK(strncmp(rsp.head, "HTTP/1.1 299 Custom Reason\r\n", 28) == 0);
    CHECK(strstr(rsp.head, "\r\nDate: yesterday\r\n") != NULL);
    CHECK(strstr(rsp.head, " GMT") == NULL);
    CHECK(strstr(rsp.head, "\r\nContent-Length: 2\r\n") != NULL);
    CHECK(strstr(rsp.head, "9999") == NULL && strstr(rsp.head, "chunked") == NULL);
    CHECK(strstr(rsp.head, "\r\nConnection: keep-alive\r\n") != NULL);
    CHECK(strstr(rsp.head, "\r\nX-Custom: 1\r\n") != NULL);
    CHECK(strstr(rsp.head, "Set-Cookie") == NULL);

    /* the ways a connection ends after the response */
    static const char *closing[] = {
        "GET /hello HTTP/1.0\r\n\r\n",                                  /* 1.0 without keep-alive */
        "GET /hello HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n",
        "GET /close HTTP/1.1\r\nHost: test\r\n\r\n",                    /* the handler asked for it */
    };
    for (int i = 0; i < 3; ++i) {
        c = client_new(&tcp_addr);
        client_puts(c, closing[i]);
        CHECK(client_read(c, &rsp, false, 3.0));
        CHECK_EQ(rsp.status, 200);
        CHECK(strstr(rsp.head, "\r\nConnection: close\r\n") != NULL);
        CHECK(client_closed(c, 3.0));
        client_free(c);
    }
    /* 1.0 with keep-alive stays open */
    c = client_new(&tcp_addr);
    client_puts(c, "GET /hello HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    CHECK(client_read(c, &rsp, false, 3.0));
    CHECK(strstr(rsp.head, "\r\nConnection: keep-alive\r\n") != NULL);
    client_puts(c, "GET /hello HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    CHECK(client_read(c, &rsp, false, 3.0));
    client_free(c);

    /* a response far bigger than the socket buffer, on a connection that is
     * then closed: all of it has to arrive first */
    c = client_new(&tcp_addr);
    client_puts(c, "GET /big HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    CHECK(client_read(c, &rsp, false, 10.0));
    CHECK_EQ(rsp.status, 200);
    CHECK_EQ(rsp.body_size, BIG_SIZE);
    CHECK(memcmp(rsp.body, big_body, BIG_SIZE) == 0);
    CHECK(client_closed(c, 3.0));
    client_free(c);

    /* answering later, by id */
    c = client_new(&tcp_addr);
    client_puts(c, "GET /async HTTP/1.1\r\nHost: test\r\n\r\n");
    client_pump(c, 0.02);
    CHECK_EQ(c->len, 0);                                /* nothing yet */
    CHECK(client_read(c, &rsp, false, 3.0));
    CHECK_EQ(rsp.status, 200);
    CHECK(strcmp(rsp.body, "later\n") == 0);
    client_puts(c, "GET /hello HTTP/1.1\r\nHost: test\r\n\r\n");    /* still usable */
    CHECK(client_read(c, &rsp, false, 3.0));
    client_free(c);
    CHECK_EQ(async_fail_count, 0);
    /* the client leaves before the answer is ready: the reply finds nobody */
    c = client_new(&tcp_addr);
    client_puts(c, "GET /async HTTP/1.1\r\nHost: test\r\n\r\n");
    client_pump(c, 0.02);
    client_free(c);
    ut_run_for(0.2);
    CHECK_EQ(async_fail_count, 1);

    /* two requests at once, answered right away: both are served */
    c = client_new(&tcp_addr);
    client_puts(c, "GET /hello HTTP/1.1\r\nHost: test\r\n\r\nGET /nothing HTTP/1.1\r\nHost: test\r\n\r\n");
    CHECK(client_read(c, &rsp, false, 3.0));
    CHECK_EQ(rsp.status, 200);
    CHECK(client_read(c, &rsp, false, 3.0));
    CHECK_EQ(rsp.status, 404);
    client_free(c);
    /* pipelining behind a request that is answered later is not supported:
     * the second request is dropped and the connection ends with the first
     * answer */
    int before = request_count;
    c = client_new(&tcp_addr);
    client_puts(c, "GET /async HTTP/1.1\r\nHost: test\r\n\r\nGET /hello HTTP/1.1\r\nHost: test\r\n\r\n");
    CHECK(client_read(c, &rsp, false, 3.0));
    CHECK(strcmp(rsp.body, "later\n") == 0);
    CHECK(strstr(rsp.head, "\r\nConnection: close\r\n") != NULL);
    CHECK(client_closed(c, 3.0));
    CHECK_EQ(request_count, before + 1);
    client_free(c);

    /* Expect: 100-continue: the go-ahead comes before the body is sent */
    c = client_new(&tcp_addr);
    client_puts(c, "POST /echo HTTP/1.1\r\nHost: test\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\n");
    CHECK(client_read(c, &rsp, true, 3.0));
    CHECK_EQ(rsp.status, 100);
    CHECK(strcmp(rsp.head, "HTTP/1.1 100 Continue\r\n\r\n") == 0);
    client_puts(c, "body");
    CHECK(client_read(c, &rsp, false, 3.0));
    CHECK_EQ(rsp.status, 200);
    CHECK(strcmp(rsp.body, "body") == 0);
    client_free(c);

    /* refusals: a response that says why, then the connection is closed */
    expect_refused("GET / HTTP/1.1\r\n\r\n", 400);
    expect_refused("GET / HTTP/1.1\nHost: test\n\n", 400);
    expect_refused("\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03", 400);
    expect_refused("POST / HTTP/1.1\r\nHost: test\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 400);
    expect_refused("POST / HTTP/1.1\r\nHost: test\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\na", 400);
    expect_refused("POST / HTTP/1.1\r\nHost: test\r\nTransfer-Encoding: gzip\r\n\r\n", 501);
    expect_refused("GET / HTTP/3.0\r\nHost: test\r\n\r\n", 505);
    /* too big for max_request_size (4096): refused as soon as it is known,
     * the body is not waited for */
    expect_refused("POST /echo HTTP/1.1\r\nHost: test\r\nContent-Length: 5000\r\n\r\n", 413);
    expect_refused("POST /echo HTTP/1.1\r\nHost: test\r\nTransfer-Encoding: chunked\r\n\r\n2000\r\n", 413);
    char huge[8192];
    int len = sprintf(huge, "GET / HTTP/1.1\r\nHost: test\r\nX-Big: ");
    memset(huge + len, 'x', 5000);
    huge[len + 5000] = 0;
    expect_refused(huge, 431);                          /* a head that never ends */
    len = sprintf(huge, "GET / HTTP/1.1\r\nHost: test\r\n");
    for (int i = 0; i < 70; ++i)
        len += sprintf(huge + len, "X-%d: 1\r\n", i);
    strcpy(huge + len, "\r\n");
    expect_refused(huge, 431);                          /* too many headers */
    /* a request of exactly max_request_size is fine */
    len = sprintf(huge, "POST /echo HTTP/1.1\r\nHost: test\r\nContent-Length: 4000\r\nX-Pad: ");
    int pad = 4096 - 4000 - len - 4;
    memset(huge + len, 'p', pad);
    len += pad;
    len += sprintf(huge + len, "\r\n\r\n");
    memset(huge + len, 'b', 4000);
    huge[len + 4000] = 0;
    CHECK_EQ(strlen(huge), 4096);
    CHECK_EQ(simple(&tcp_addr, huge, &rsp), 200);
    CHECK_EQ(rsp.body_size, 4000);

    /* a refusal while the client is still sending: the response must not
     * be lost to a connection reset */
    c = client_new(&tcp_addr);
    client_puts(c, "POST /echo HTTP/1.1\r\nHost: test\r\nContent-Length: 100000\r\n\r\n");
    for (int i = 0; i < 20; ++i) {
        send(c->fd, big_body, 5000, MSG_NOSIGNAL | MSG_DONTWAIT);
        ut_run_for(0.002);
    }
    CHECK(client_read(c, &rsp, false, 3.0));
    CHECK_EQ(rsp.status, 413);
    client_free(c);

    /* many requests on one connection, then everything is given back */
    c = client_new(&tcp_addr);
    for (int i = 0; i < 500; ++i) {
        client_puts(c, "POST /echo HTTP/1.1\r\nHost: test\r\nContent-Length: 3\r\n\r\nabc");
        CHECK(client_read(c, &rsp, false, 3.0));
        CHECK_EQ(rsp.status, 200);
    }
    client_free(c);
    ut_run_for(0.05);
    CHECK_EQ(svr->raw_svr->clt_count, 0);

    /* release with a connection waiting for an answer and an idle one */
    struct client *waiting = client_new(&tcp_addr);
    struct client *idle = client_new(&tcp_addr);
    client_puts(waiting, "GET /never HTTP/1.1\r\nHost: test\r\n\r\n");
    client_pump(waiting, 0.05);
    CHECK_EQ(svr->raw_svr->clt_count, 2);
    nw_http_svr_release(svr);
    CHECK(client_closed(waiting, 1.0));
    CHECK(client_closed(idle, 1.0));
    client_free(waiting);
    client_free(idle);

    /*
     * timeouts, on a server with short ones
     */
    cfg.bind_count = 1;
    cfg.keep_alive_timeout = 0.5;
    cfg.request_timeout = 0.3;
    svr = nw_http_svr_create(&cfg, &st, NULL);
    CHECK(svr != NULL);
    CHECK_EQ(nw_http_svr_start(svr), 0);
    CHECK_EQ(nw_sock_host_addr(svr->raw_svr->bind_list[0].sockfd, &tcp_addr), 0);

    struct client *slow = client_new(&tcp_addr);        /* never finishes its request */
    idle = client_new(&tcp_addr);                       /* connects and says nothing */
    struct client *done = client_new(&tcp_addr);        /* one request, then silence */
    waiting = client_new(&tcp_addr);                    /* its request is never answered */
    client_puts(slow, "GET /hello HTTP/1.1\r\nHost: te");
    client_puts(done, "GET /hello HTTP/1.1\r\nHost: test\r\n\r\n");
    client_puts(waiting, "GET /never HTTP/1.1\r\nHost: test\r\n\r\n");
    CHECK(client_read(done, &rsp, false, 3.0));
    CHECK_EQ(rsp.status, 200);

    CHECK(client_read(slow, &rsp, false, 4.0));         /* 408, then closed */
    CHECK_EQ(rsp.status, 408);
    CHECK(client_closed(slow, 4.0));
    CHECK(client_closed(idle, 4.0));                    /* closed without a word */
    CHECK(client_closed(done, 4.0));
    client_pump(waiting, 1.5);
    CHECK(!waiting->eof);                               /* a pending request is not timed out */
    /* the refused connection lingers until its client hangs up */
    client_free(slow);
    client_free(idle);
    client_free(done);
    ut_run_for(0.05);
    CHECK_EQ(svr->raw_svr->clt_count, 1);
    client_free(waiting);
    ut_run_for(0.05);
    CHECK_EQ(svr->raw_svr->clt_count, 0);
    nw_http_svr_release(svr);

    free(big_body);
    return 0;
}
