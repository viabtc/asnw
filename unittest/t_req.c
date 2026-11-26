/*
 * nw_req: GET and POST with a real http server (nw_http, in this loop) and
 * from a server that answers with whatever a test needs: chunked bodies,
 * bodies that end with the connection, broken and missing responses.
 * Then what can go wrong on the way: urls that are refused, a port
 * nobody listens on, a server that never answers, requests that are
 * cancelled or still on their way when the loop is released.
 */
# include <stdarg.h>
# include <pthread.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# include "common.h"
# include "nw_req.h"
# include "nw_http.h"

# define BIG_SIZE (3 * 1024 * 1024)
# define CHUNKS 400

static int http_port, raw_port, silent_port, dead_port;
static char *big;
static char *chunked_body;
static size_t chunked_size;

/* the real server */
static void on_request(nw_ses *ses, nw_http_request *req)
{
    char body[1024];
    if (strcmp(req->path, "/missing") == 0) {
        CHECK_EQ(nw_http_reply_text(ses, 404, "text/plain", "nope", 4), 0);
    } else if (strcmp(req->path, "/empty") == 0) {
        CHECK_EQ(nw_http_reply_text(ses, 204, NULL, NULL, 0), 0);
    } else if (strcmp(req->path, "/post/echo") == 0) {
        /* the body as it came */
        CHECK(strcmp(req->method, "POST") == 0);
        CHECK_EQ(nw_http_reply_text(ses, 200, "application/octet-stream", req->body, req->body_size), 0);
    } else if (strcmp(req->method, "POST") == 0) {
        char *type = nw_http_get_header(req, "Content-Type");
        char *length = nw_http_get_header(req, "Content-Length");
        char *host = nw_http_get_header(req, "Host");
        int len = snprintf(body, sizeof(body), "POST path=%s query=%s host=%s type=%s length=%s body=%.*s",
                req->path, req->query ? req->query : "", host ? host : "-", type ? type : "-",
                length ? length : "-", (int)(req->body_size < 200 ? req->body_size : 200), (char *)req->body);
        CHECK_EQ(nw_http_reply_text(ses, 200, "text/plain", body, len), 0);
    } else if (strcmp(req->path, "/big") == 0) {
        CHECK_EQ(nw_http_reply_text(ses, 200, "application/octet-stream", big, BIG_SIZE), 0);
    } else {
        char *host = nw_http_get_header(req, "Host");
        char *agent = nw_http_get_header(req, "User-Agent");
        char *connection = nw_http_get_header(req, "Connection");
        CHECK(nw_http_get_header(req, "Content-Length") == NULL && nw_http_get_header(req, "Content-Type") == NULL);
        CHECK_EQ(req->body_size, 0);
        int len = snprintf(body, sizeof(body), "%s path=%s query=%s host=%s agent=%s connection=%s",
                req->method, req->path, req->query ? req->query : "", host ? host : "-",
                agent ? agent : "-", connection ? connection : "-");
        nw_http_response rsp;
        memset(&rsp, 0, sizeof(rsp));
        rsp.status = 200;
        nw_http_add_header(&rsp, "Content-Type", "text/plain");
        nw_http_add_header(&rsp, "X-Test", "some value");
        rsp.body = body;
        rsp.body_size = len;
        CHECK_EQ(nw_http_reply(ses, &rsp), 0);
    }
}

/* the server that says what it is told to */
static int raw_decode(nw_ses *ses, void *data, size_t max)
{
    char *s = data;
    for (size_t i = 0; i + 3 < max; ++i) {
        if (memcmp(s + i, "\r\n\r\n", 4) == 0)
            return i + 4;
    }
    return 0;
}

static struct {
    const char *path;
    const char *response;
} canned[] = {
    { "/chunked",   "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nX-Test:  padded  \r\n\r\n"
                    "5\r\nhello\r\n7;ext=1\r\n, world\r\nA\r\n0123456789\r\n0\r\nTrailer: x\r\n\r\n" },
    { "/eof",       "HTTP/1.0 200 OK\r\nServer: old\r\n\r\nuntil the end" },
    { "/continue",  "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok" },
    { "/extra",     "HTTP/1.1 200 OK\r\ncontent-length: 4\r\n\r\nfourand more" },
    { "/redirect",  "HTTP/1.1 301 Moved Permanently\r\nLocation: http://elsewhere/\r\nContent-Length: 0\r\n\r\n" },
    { "/notmod",    "HTTP/1.1 304 Not Modified\r\nETag: \"abc\"\r\n\r\n" },
    { "/noreason",  "HTTP/1.1 200\r\nContent-Length: 1\r\n\r\nx" },
    { "/short",     "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly this" },
    { "/shortchunk","HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n" },
    { "/halfhead",  "HTTP/1.1 200 OK\r\nContent-Le" },
    { "/garbage",   "SSH-2.0-OpenSSH_9.6\r\n\r\n" },
    { "/badlen",    "HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\nabc" },
    { "/badchunk",  "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nhello\r\n0\r\n\r\n" },
    { "/badchunk2", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloXX0\r\n\r\n" },
    { "/huge",      "HTTP/1.1 200 OK\r\nContent-Length: 99999999999\r\n\r\nx" },
    { "/hugechunk", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFF\r\nx" },
    { "/closed",    "" },
};

static void raw_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    char line[256];
    char path[128] = "";
    snprintf(line, sizeof(line), "%.*s", (int)(size < 200 ? size : 200), (char *)data);
    char method[8] = "";
    CHECK_EQ(sscanf(line, "%7s %127s HTTP/1.1", method, path), 2);
    CHECK(strcmp(method, "GET") == 0 || strcmp(method, "POST") == 0);

    if (strcmp(path, "/manychunks") == 0) {
        /* sent piece by piece, so that it is also read piece by piece */
        const char *head = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
        CHECK_EQ(nw_ses_send(ses, head, strlen(head)), 0);
        size_t at = 0;
        for (int i = 0; i < CHUNKS; ++i) {
            size_t n = (i * 37) % 3000 + 1;
            char size_line[32];
            int len = snprintf(size_line, sizeof(size_line), i % 2 ? "%zx\r\n" : "%zX\r\n", n);
            CHECK_EQ(nw_ses_send(ses, size_line, len), 0);
            CHECK_EQ(nw_ses_send(ses, chunked_body + at, n), 0);
            CHECK_EQ(nw_ses_send(ses, "\r\n", 2), 0);
            at += n;
        }
        CHECK_EQ(at, chunked_size);
        CHECK_EQ(nw_ses_send(ses, "0\r\n\r\n", 5), 0);
        nw_svr_shutdown_clt(ses->svr, ses);
        return;
    }
    for (size_t i = 0; i < sizeof(canned) / sizeof(canned[0]); ++i) {
        if (strcmp(path, canned[i].path) == 0) {
            if (canned[i].response[0])
                CHECK_EQ(nw_ses_send(ses, canned[i].response, strlen(canned[i].response)), 0);
            nw_svr_shutdown_clt(ses->svr, ses);
            return;
        }
    }
    CHECK(0);
}

/* a request and what came back */
struct result {
    int calls;
    int error;
    int status;
    uint32_t headers;
    size_t size;
    char *body;
    char message[256];
    char x_test[64];
    pthread_t thread;
};

static void on_response(int error, nw_req_rsp *rsp, void *privdata)
{
    struct result *r = privdata;
    r->calls++;
    r->error = error;
    r->status = rsp->status;
    r->headers = rsp->header_count;
    r->size = rsp->body_size;
    r->thread = pthread_self();
    CHECK(rsp->body != NULL && rsp->message != NULL);
    CHECK_EQ(rsp->body[rsp->body_size], 0);
    CHECK((error != 0) == (rsp->message[0] != 0));
    if (error) {
        CHECK_EQ(rsp->status, 0);
        CHECK_EQ(rsp->body_size, 0);
        CHECK_EQ(rsp->header_count, 0);
    }
    r->body = malloc(rsp->body_size + 1);
    memcpy(r->body, rsp->body, rsp->body_size + 1);
    snprintf(r->message, sizeof(r->message), "%s", rsp->message);
    /* whatever the case of the name */
    const char *value = nw_req_get_header(rsp, "x-TEST");
    snprintf(r->x_test, sizeof(r->x_test), "%s", value ? value : "");
    CHECK(nw_req_get_header(rsp, "No-Such-Header") == NULL);
}

static void result_reset(struct result *r)
{
    free(r->body);
    memset(r, 0, sizeof(*r));
}

static void wait_for(struct result *r)
{
    for (int i = 0; i < 3000 && r->calls == 0; ++i)
        ut_run_for(0.01);
    CHECK_EQ(r->calls, 1);
}

static void fetch(struct result *r, double timeout, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void fetch(struct result *r, double timeout, const char *fmt, ...)
{
    char url[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(url, sizeof(url), fmt, ap);
    va_end(ap);
    result_reset(r);
    CHECK(nw_req_get(url, timeout, on_response, r) != NULL);
    wait_for(r);
    if (r->error)
        fprintf(stderr, "%-28s error %d: %s\n", strrchr(url, '/') ? strrchr(url, '/') : url, r->error, r->message);
}

static void post(struct result *r, double timeout, const char *content_type, const void *body, size_t size,
        const char *fmt, ...) __attribute__((format(printf, 6, 7)));
static void post(struct result *r, double timeout, const char *content_type, const void *body, size_t size,
        const char *fmt, ...)
{
    char url[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(url, sizeof(url), fmt, ap);
    va_end(ap);
    result_reset(r);
    CHECK(nw_req_post(url, content_type, body, size, timeout, on_response, r) != NULL);
    wait_for(r);
    if (r->error)
        fprintf(stderr, "POST %-23s error %d: %s\n", strrchr(url, '/') ? strrchr(url, '/') : url, r->error, r->message);
}

# define CHECK_BODY(r, text) do { \
    CHECK_EQ((r)->error, 0); \
    CHECK_EQ((r)->size, strlen(text)); \
    CHECK(strcmp((r)->body, text) == 0); \
} while (0)

static int listen_on_loopback(int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK_EQ(bind(fd, (struct sockaddr *)&in, sizeof(in)), 0);
    CHECK_EQ(listen(fd, 64), 0);
    socklen_t len = sizeof(in);
    CHECK_EQ(getsockname(fd, (struct sockaddr *)&in, &len), 0);
    *port = ntohs(in.sin_port);
    return fd;
}

/* a request that asks for the next one from its callback */
static int chain_left;
static struct result chain_result;

static void on_chain(int error, nw_req_rsp *rsp, void *privdata)
{
    CHECK_EQ(error, 0);
    CHECK_EQ(rsp->status, 200);
    if (--chain_left > 0) {
        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/chain?left=%d", http_port, chain_left);
        CHECK(nw_req_get(url, 5.0, on_chain, NULL) != NULL);
    } else {
        on_response(error, rsp, &chain_result);
    }
}

/* another loop thread has request threads of its own, gone with its loop */
static int other_done;

static void on_other_response(int error, nw_req_rsp *rsp, void *privdata)
{
    on_response(error, rsp, privdata);
    nw_loop_break();
}

static void on_other_timeout(nw_timer *timer, void *privdata)
{
    nw_loop_break();
}

static void *other_thread(void *arg)
{
    struct result *r = arg;
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/other?thread=2", http_port);
    for (int i = 0; i < 3; ++i) {
        r->calls = 0;
        free(r->body);
        r->body = NULL;
        if (nw_req_get(url, 10.0, on_other_response, r) == NULL)
            abort();
        nw_timer timer;
        nw_timer_set(&timer, 20.0, false, on_other_timeout, NULL);
        nw_timer_start(&timer);
        nw_loop_run();
        nw_timer_stop(&timer);
        if (r->calls != 1)
            abort();
        /* the last round leaves a request on its way for the release */
        if (i == 1) {
            snprintf(url, sizeof(url), "http://127.0.0.1:%d/", silent_port);
            if (nw_req_get(url, 30.0, on_other_response, r) == NULL)
                abort();
            snprintf(url, sizeof(url), "http://127.0.0.1:%d/other?thread=2", http_port);
        }
    }
    nw_loop_release();
    __sync_fetch_and_add(&other_done, 1);
    return NULL;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    struct result r;
    memset(&r, 0, sizeof(r));
    char text[1024];
    bool slow = getenv("ASNW_UT_VALGRIND") != NULL;
    double begin;

    /* urls that are not for nw_req: refused at once, no callback */
    const char *bad[] = {
        "", "127.0.0.1/", "https://127.0.0.1/", "ftp://127.0.0.1/", "http:/127.0.0.1/", "http://", "http:///path",
        "http://:80/", "http://host:/", "http://host:0/", "http://host:65536/", "http://host:80x/", "http://host:1234567/",
        "http://user@host/", "http://user:secret@host/", "http://ho st/", "http://host/a b", "http://host/a\r\nX-Evil: 1",
        "http://host/a\tb", "http://[::1/", "http://[::1]x/", "http://[]/", "http://[::g]/", "http://host\\path",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        if (nw_req_get(bad[i], 1.0, on_response, &r) != NULL) {
            fprintf(stderr, "accepted: %s\n", bad[i]);
            exit(1);
        }
    }
    char *long_url = malloc(NW_REQ_URL_MAX + 16);
    memset(long_url, 'a', NW_REQ_URL_MAX + 8);
    memcpy(long_url, "http://host/", 12);
    long_url[NW_REQ_URL_MAX] = '\0';
    CHECK(nw_req_get(long_url, 1.0, on_response, &r) == NULL);
    free(long_url);

    /* the servers */
    static nw_svr_bind bind_http;
    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1:0", &bind_http.addr, &bind_http.sock_type), 0);
    nw_http_svr_cfg hc;
    memset(&hc, 0, sizeof(hc));
    hc.bind_count = 1;
    hc.bind_arr = &bind_http;
    hc.max_request_size = 2 * 1024 * 1024;
    nw_http_svr_type ht;
    memset(&ht, 0, sizeof(ht));
    ht.on_request = on_request;
    nw_http_svr *http = nw_http_svr_create(&hc, &ht, NULL);
    CHECK(http != NULL);
    CHECK_EQ(nw_http_svr_start(http), 0);
    http_port = ut_svr_port(http->raw_svr, 0);

    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = raw_decode;
    st.on_recv_pkg = raw_on_recv_pkg;
    nw_svr *raw = ut_svr("tcp@127.0.0.1:0", &st, 4096, NULL);
    raw_port = ut_svr_port(raw, 0);

    /* connections to this one are taken by the system and never answered */
    int silent_fd = listen_on_loopback(&silent_port);
    /* and nobody listens on that one */
    close(listen_on_loopback(&dead_port));

    big = malloc(BIG_SIZE);
    for (int i = 0; i < BIG_SIZE; ++i)
        big[i] = (char)(i * 31 + i / 251);
    for (int i = 0; i < CHUNKS; ++i)
        chunked_size += (i * 37) % 3000 + 1;
    chunked_body = malloc(chunked_size);
    for (size_t i = 0; i < chunked_size; ++i)
        chunked_body[i] = (char)(i * 7 + i / 255);

    /* a plain request: what the server sees, and what comes back */
    fetch(&r, 5.0, "http://127.0.0.1:%d/echo/sub?k=v&z=%%20#fragment", http_port);
    snprintf(text, sizeof(text), "GET path=/echo/sub query=k=v&z=%%20 host=127.0.0.1:%d agent=asnw connection=close", http_port);
    CHECK_BODY(&r, text);
    CHECK_EQ(r.status, 200);
    CHECK(strcmp(r.x_test, "some value") == 0);
    CHECK(r.headers >= 3);
    CHECK(pthread_equal(r.thread, pthread_self()));
    /* no path at all, and a query without a path */
    fetch(&r, 5.0, "http://127.0.0.1:%d", http_port);
    snprintf(text, sizeof(text), "GET path=/ query= host=127.0.0.1:%d agent=asnw connection=close", http_port);
    CHECK_BODY(&r, text);
    fetch(&r, 5.0, "HTTP://127.0.0.1:%d?x=1", http_port);
    snprintf(text, sizeof(text), "GET path=/ query=x=1 host=127.0.0.1:%d agent=asnw connection=close", http_port);
    CHECK_BODY(&r, text);
    /* by name. localhost may be ::1 first, where nobody listens */
    fetch(&r, 5.0, "http://localhost:%d/named", http_port);
    snprintf(text, sizeof(text), "GET path=/named query= host=localhost:%d agent=asnw connection=close", http_port);
    CHECK_BODY(&r, text);
    /* 0 is the default timeout */
    fetch(&r, 0, "http://127.0.0.1:%d/default", http_port);
    CHECK_EQ(r.error, 0);

    /* a status is not an error */
    fetch(&r, 5.0, "http://127.0.0.1:%d/missing", http_port);
    CHECK_EQ(r.status, 404);
    CHECK_BODY(&r, "nope");
    fetch(&r, 5.0, "http://127.0.0.1:%d/empty", http_port);
    CHECK_EQ(r.status, 204);
    CHECK_BODY(&r, "");
    fetch(&r, 5.0, "http://127.0.0.1:%d/redirect", raw_port);
    CHECK_EQ(r.status, 301);
    CHECK_BODY(&r, "");
    fetch(&r, 5.0, "http://127.0.0.1:%d/notmod", raw_port);
    CHECK_EQ(r.status, 304);
    CHECK_BODY(&r, "");

    /* a body of some megabytes */
    fetch(&r, 20.0, "http://127.0.0.1:%d/big", http_port);
    CHECK_EQ(r.error, 0);
    CHECK_EQ(r.size, BIG_SIZE);
    CHECK(memcmp(r.body, big, BIG_SIZE) == 0);

    /* the other ways a body ends */
    fetch(&r, 5.0, "http://127.0.0.1:%d/chunked", raw_port);
    CHECK_BODY(&r, "hello, world0123456789");
    CHECK(strcmp(r.x_test, "padded") == 0);
    fetch(&r, 20.0, "http://127.0.0.1:%d/manychunks", raw_port);
    CHECK_EQ(r.error, 0);
    CHECK_EQ(r.size, chunked_size);
    CHECK(memcmp(r.body, chunked_body, chunked_size) == 0);
    fetch(&r, 5.0, "http://127.0.0.1:%d/eof", raw_port);
    CHECK_BODY(&r, "until the end");
    fetch(&r, 5.0, "http://127.0.0.1:%d/continue", raw_port);
    CHECK_EQ(r.status, 200);
    CHECK_BODY(&r, "ok");
    fetch(&r, 5.0, "http://127.0.0.1:%d/extra", raw_port);
    CHECK_BODY(&r, "four");
    fetch(&r, 5.0, "http://127.0.0.1:%d/noreason", raw_port);
    CHECK_BODY(&r, "x");

    /* POST: the body and what it is, json unless told otherwise */
    const char *alert = "{\"alert\": \"disk full\", \"host\": \"db-3\"}";
    post(&r, 5.0, NULL, alert, strlen(alert), "http://127.0.0.1:%d/alerts?level=2", http_port);
    snprintf(text, sizeof(text), "POST path=/alerts query=level=2 host=127.0.0.1:%d type=application/json length=%zu body=%s",
            http_port, strlen(alert), alert);
    CHECK_BODY(&r, text);
    CHECK_EQ(r.status, 200);
    post(&r, 5.0, "text/plain; charset=utf-8", "plain words", 11, "http://localhost:%d/notes", http_port);
    snprintf(text, sizeof(text), "POST path=/notes query= host=localhost:%d type=text/plain; charset=utf-8 length=11 body=plain words", http_port);
    CHECK_BODY(&r, text);
    /* the body is copied: the caller's may go at once */
    char *gone = strdup("soon overwritten");
    result_reset(&r);
    snprintf(text, sizeof(text), "http://127.0.0.1:%d/copied", http_port);
    CHECK(nw_req_post(text, "text/plain", gone, strlen(gone), 5.0, on_response, &r) != NULL);
    memset(gone, 'x', strlen(gone));
    free(gone);
    wait_for(&r);
    snprintf(text, sizeof(text), "POST path=/copied query= host=127.0.0.1:%d type=text/plain length=16 body=soon overwritten", http_port);
    CHECK_BODY(&r, text);
    /* no body at all is a body of no bytes */
    post(&r, 5.0, NULL, NULL, 0, "http://127.0.0.1:%d/ping", http_port);
    snprintf(text, sizeof(text), "POST path=/ping query= host=127.0.0.1:%d type=application/json length=0 body=", http_port);
    CHECK_BODY(&r, text);
    post(&r, 5.0, "text/plain", "", 0, "http://127.0.0.1:%d/ping", http_port);
    CHECK_EQ(r.error, 0);
    /* a megabyte of anything, zero bytes included, comes back the same */
    enum { POST_SIZE = 1024 * 1024 + 7 };
    post(&r, 20.0, "application/octet-stream", big, POST_SIZE, "http://127.0.0.1:%d/post/echo", http_port);
    CHECK_EQ(r.error, 0);
    CHECK_EQ(r.size, POST_SIZE);
    CHECK(memcmp(r.body, big, POST_SIZE) == 0);
    /* the status is the server's to choose */
    post(&r, 5.0, NULL, "{}", 2, "http://127.0.0.1:%d/missing", http_port);
    CHECK_EQ(r.status, 404);
    CHECK_BODY(&r, "nope");
    /* a server that takes the request and leaves */
    post(&r, 5.0, NULL, "{}", 2, "http://127.0.0.1:%d/closed", raw_port);
    CHECK_EQ(r.error, NW_REQ_ERR_IO);
    /* and one that never reads: more than the system keeps for it */
    char *huge = calloc(1, NW_REQ_MAX_SIZE + 1);
    begin = ev_time();
    post(&r, 0.5, "application/octet-stream", huge, NW_REQ_MAX_SIZE, "http://127.0.0.1:%d/", silent_port);
    CHECK_EQ(r.error, NW_REQ_ERR_TIMEOUT);
    CHECK(ev_time() - begin < (slow ? 20.0 : 5.0));
    /* what is refused at once */
    snprintf(text, sizeof(text), "http://127.0.0.1:%d/refused", http_port);
    result_reset(&r);
    CHECK(nw_req_post(text, NULL, huge, NW_REQ_MAX_SIZE + 1, 1.0, on_response, &r) == NULL);
    CHECK(nw_req_post(text, NULL, NULL, 5, 1.0, on_response, &r) == NULL);
    CHECK(nw_req_post(text, "", "x", 1, 1.0, on_response, &r) == NULL);
    CHECK(nw_req_post(text, "text/plain\r\nX-Evil: 1", "x", 1, 1.0, on_response, &r) == NULL);
    CHECK(nw_req_post(text, "text/plain\nX-Evil: 1", "x", 1, 1.0, on_response, &r) == NULL);
    CHECK(nw_req_post("https://127.0.0.1/", NULL, "x", 1, 1.0, on_response, &r) == NULL);
    free(huge);
    ut_run_for(0.1);
    CHECK_EQ(r.calls, 0);

    /* responses that are not */
    static const struct {
        const char *path;
        int error;
    } broken[] = {
        { "/short", NW_REQ_ERR_IO }, { "/shortchunk", NW_REQ_ERR_IO }, { "/closed", NW_REQ_ERR_IO },
        { "/halfhead", NW_REQ_ERR_RESPONSE }, { "/garbage", NW_REQ_ERR_RESPONSE }, { "/badlen", NW_REQ_ERR_RESPONSE },
        { "/badchunk", NW_REQ_ERR_RESPONSE }, { "/badchunk2", NW_REQ_ERR_RESPONSE },
        { "/huge", NW_REQ_ERR_TOO_LARGE }, { "/hugechunk", NW_REQ_ERR_TOO_LARGE },
    };
    for (size_t i = 0; i < sizeof(broken) / sizeof(broken[0]); ++i) {
        fetch(&r, 5.0, "http://127.0.0.1:%d%s", raw_port, broken[i].path);
        if (r.error != broken[i].error) {
            fprintf(stderr, "%s: error %d, expected %d\n", broken[i].path, r.error, broken[i].error);
            exit(1);
        }
    }

    /* nobody there */
    fetch(&r, 5.0, "http://127.0.0.1:%d/", dead_port);
    CHECK_EQ(r.error, NW_REQ_ERR_CONNECT);
    snprintf(text, sizeof(text), "connect to 127.0.0.1:%d: ", dead_port);
    CHECK(strncmp(r.message, text, strlen(text)) == 0);
    fetch(&r, 5.0, "http://[::1]:%d/", dead_port);
    CHECK_EQ(r.error, NW_REQ_ERR_CONNECT);

    /* a server that never answers: the timeout, and not much later */
    begin = ev_time();
    fetch(&r, 0.3, "http://127.0.0.1:%d/", silent_port);
    CHECK_EQ(r.error, NW_REQ_ERR_TIMEOUT);
    CHECK(ev_time() - begin >= 0.25);
    CHECK(ev_time() - begin < (slow ? 10.0 : 2.0));

    /* both threads busy: a request that waits too long for one of them
     * is not sent at all */
    struct result busy[2];
    memset(busy, 0, sizeof(busy));
    snprintf(text, sizeof(text), "http://127.0.0.1:%d/", silent_port);
    CHECK(nw_req_get(text, 0.6, on_response, &busy[0]) != NULL);
    CHECK(nw_req_get(text, 0.6, on_response, &busy[1]) != NULL);
    fetch(&r, 0.2, "http://127.0.0.1:%d/late", http_port);
    CHECK_EQ(r.error, NW_REQ_ERR_TIMEOUT);
    wait_for(&busy[0]);
    wait_for(&busy[1]);
    CHECK_EQ(busy[0].error, NW_REQ_ERR_TIMEOUT);
    CHECK_EQ(busy[1].error, NW_REQ_ERR_TIMEOUT);

    /* cancelled: no callback, and the threads do not wait for the timeout */
    result_reset(&busy[0]);
    result_reset(&busy[1]);
    nw_req *first = nw_req_get(text, 60.0, on_response, &busy[0]);
    nw_req *second = nw_req_get(text, 60.0, on_response, &busy[1]);
    CHECK(first != NULL && second != NULL);
    ut_run_for(0.1);
    nw_req_cancel(first);
    nw_req_cancel(second);
    begin = ev_time();
    fetch(&r, 30.0, "http://127.0.0.1:%d/after", http_port);
    CHECK_EQ(r.error, 0);
    CHECK(ev_time() - begin < (slow ? 20.0 : 3.0));
    CHECK_EQ(busy[0].calls + busy[1].calls, 0);
    /* cancelled while waiting for a thread: never sent */
    first = nw_req_get(text, 0.5, on_response, &busy[0]);
    second = nw_req_get(text, 0.5, on_response, &busy[1]);
    snprintf(text, sizeof(text), "http://127.0.0.1:%d/unwanted", http_port);
    struct result unwanted;
    memset(&unwanted, 0, sizeof(unwanted));
    nw_req *third = nw_req_get(text, 30.0, on_response, &unwanted);
    CHECK(first != NULL && second != NULL && third != NULL);
    nw_req_cancel(third);
    wait_for(&busy[0]);
    wait_for(&busy[1]);
    ut_run_for(0.2);
    CHECK_EQ(unwanted.calls, 0);

    /* many at once, each with its own answer */
    enum { MANY = 40 };
    static struct result many[MANY];
    for (int i = 0; i < MANY; ++i) {
        snprintf(text, sizeof(text), "http://127.0.0.1:%d/many?i=%d", http_port, i);
        CHECK(nw_req_get(text, 30.0, on_response, &many[i]) != NULL);
    }
    for (int i = 0; i < MANY; ++i) {
        wait_for(&many[i]);
        snprintf(text, sizeof(text), "GET path=/many query=i=%d host=127.0.0.1:%d agent=asnw connection=close", i, http_port);
        CHECK_BODY(&many[i], text);
        free(many[i].body);
    }

    /* a request made from the callback of another */
    chain_left = 5;
    snprintf(text, sizeof(text), "http://127.0.0.1:%d/chain", http_port);
    CHECK(nw_req_get(text, 5.0, on_chain, NULL) != NULL);
    wait_for(&chain_result);
    snprintf(text, sizeof(text), "GET path=/chain query=left=1 host=127.0.0.1:%d agent=asnw connection=close", http_port);
    CHECK_BODY(&chain_result, text);
    free(chain_result.body);

    /* another loop thread, released with a request on its way */
    pthread_t thread;
    struct result other;
    memset(&other, 0, sizeof(other));
    CHECK_EQ(pthread_create(&thread, NULL, other_thread, &other), 0);
    for (int i = 0; i < 3000 && !__sync_fetch_and_add(&other_done, 0); ++i)
        ut_run_for(0.01);
    CHECK_EQ(pthread_join(thread, NULL), 0);
    CHECK_EQ(other.calls, 1);
    CHECK(pthread_equal(other.thread, thread));
    snprintf(text, sizeof(text), "GET path=/other query=thread=2 host=127.0.0.1:%d agent=asnw connection=close", http_port);
    CHECK_BODY(&other, text);
    free(other.body);

    /* released with requests on their way and waiting: no callback, and
     * no waiting for their timeout */
    result_reset(&busy[0]);
    result_reset(&busy[1]);
    snprintf(text, sizeof(text), "http://127.0.0.1:%d/", silent_port);
    CHECK(nw_req_get(text, 60.0, on_response, &busy[0]) != NULL);
    CHECK(nw_req_get(text, 60.0, on_response, &busy[1]) != NULL);
    CHECK(nw_req_get(text, 60.0, on_response, &unwanted) != NULL);
    ut_run_for(0.1);
    nw_http_svr_release(http);
    nw_svr_release(raw);
    begin = ev_time();
    nw_loop_release();
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    CHECK(ts.tv_sec + ts.tv_nsec / 1e9 - begin < (slow ? 20.0 : 3.0));
    CHECK_EQ(busy[0].calls + busy[1].calls + unwanted.calls, 0);

    close(silent_fd);
    result_reset(&r);
    free(big);
    free(chunked_body);
    return 0;
}
