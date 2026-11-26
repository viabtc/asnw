/*
 * nw_http_svr against a real client: curl. The server runs in a child
 * process, curl in the parent. Skipped when curl is not installed.
 */
# include "common.h"
# include "nw_http.h"
# include <stdarg.h>
# include <sys/wait.h>
# include <time.h>

# define BIG_SIZE (1024 * 1024)

static char *big_body;

static void on_request(nw_ses *ses, nw_http_request *req)
{
    if (strcmp(req->path, "/hello") == 0) {
        nw_http_reply_body(ses, 200, "text/plain", "hello\n", 6);
    } else if (strcmp(req->path, "/echo") == 0) {
        const char *agent = nw_http_get_header(req, "user-agent");
        nw_http_response rsp;
        memset(&rsp, 0, sizeof(rsp));
        rsp.status = 200;
        nw_http_add_header(&rsp, "X-Method", req->method);
        nw_http_add_header(&rsp, "X-Path", req->path);
        nw_http_add_header(&rsp, "X-Query", req->query);
        nw_http_add_header(&rsp, "X-Agent", agent ? agent : "none");
        rsp.body = req->body;
        rsp.body_size = req->body_size;
        nw_http_reply(ses, &rsp);
    } else if (strcmp(req->path, "/big") == 0) {
        nw_http_reply_body(ses, 200, "application/octet-stream", big_body, BIG_SIZE);
    } else {
        nw_http_reply_body(ses, 404, "text/plain", "no such page\n", 13);
    }
}

static void run_server(int port, const char *unix_cfg)
{
    char tcp_cfg[64];
    snprintf(tcp_cfg, sizeof(tcp_cfg), "tcp@127.0.0.1:%d", port);
    nw_svr_bind binds[2];
    CHECK_EQ(nw_sock_cfg_parse(tcp_cfg, &binds[0].addr, &binds[0].sock_type), 0);
    CHECK_EQ(nw_sock_cfg_parse(unix_cfg, &binds[1].addr, &binds[1].sock_type), 0);
    nw_http_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 2;
    cfg.bind_arr = binds;
    nw_http_svr_type type;
    memset(&type, 0, sizeof(type));
    type.on_request = on_request;
    nw_http_svr *svr = nw_http_svr_create(&cfg, &type, NULL);
    CHECK(svr != NULL);
    CHECK_EQ(nw_http_svr_start(svr), 0);
    ut_run_for(60.0);
    _exit(0);
}

/* run a command, return what it printed */
static const char *run(const char *fmt, ...)
{
    static char out[65536];
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    FILE *fp = popen(cmd, "r");
    CHECK(fp != NULL);
    size_t len = fread(out, 1, sizeof(out) - 1, fp);
    out[len] = 0;
    int status = pclose(fp);
    if (status != 0) {
        fprintf(stderr, "command failed (%d): %s\n", status, cmd);
        exit(1);
    }
    return out;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void)
{
    if (system("curl --version > /dev/null 2>&1") != 0) {
        fprintf(stderr, "skipped: curl is not installed\n");
        return 0;
    }

    big_body = malloc(BIG_SIZE);
    for (int i = 0; i < BIG_SIZE; ++i)
        big_body[i] = 'a' + i % 23;

    int port = ut_free_port(SOCK_STREAM);
    char unix_cfg[256], url[64], file[128];
    ut_unix_cfg(unix_cfg, sizeof(unix_cfg), "stream", "curl.sock");
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", port);
    snprintf(file, sizeof(file), "%s/upload", ut_tmpdir());

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0)
        run_server(port, unix_cfg);
    usleep(200 * 1000);

    const char *out;
    out = run("curl -sS %s/hello", url);
    CHECK(strcmp(out, "hello\n") == 0);

    out = run("curl -sS -o /dev/null -w '%%{http_code}' %s/nothing", url);
    CHECK(strcmp(out, "404") == 0);

    /* a form post, with a query that needs decoding on the path side */
    out = run("curl -sS -i -A test-agent -d 'a=1&b=2' '%s/ec%%68o?x=%%20y'", url);
    CHECK(strstr(out, "HTTP/1.1 200 OK\r\n") == out);
    CHECK(strstr(out, "\r\nX-Method: POST\r\n") != NULL);
    CHECK(strstr(out, "\r\nX-Path: /echo\r\n") != NULL);
    CHECK(strstr(out, "\r\nX-Query: x=%20y\r\n") != NULL);
    CHECK(strstr(out, "\r\nX-Agent: test-agent\r\n") != NULL);
    CHECK(strstr(out, "\r\n\r\na=1&b=2") != NULL);

    /* a body big enough for curl to send "Expect: 100-continue" and wait for
     * the go-ahead: without it curl stalls for a second before sending */
    FILE *fp = fopen(file, "w");
    CHECK(fp != NULL);
    CHECK_EQ(fwrite(big_body, 1, 40000, fp), 40000);
    fclose(fp);
    double start = now();
    out = run("curl -sS -H 'Expect: 100-continue' --data-binary @%s %s/echo | cmp - %s && echo same", file, url, file);
    CHECK(strcmp(out, "same\n") == 0);
    fprintf(stderr, "40000 byte upload with 100-continue: %.3fs\n", now() - start);
    CHECK(now() - start < 0.8);

    /* chunked upload */
    out = run("curl -sS -H 'Transfer-Encoding: chunked' --data-binary @%s %s/echo | cmp - %s && echo same", file, url, file);
    CHECK(strcmp(out, "same\n") == 0);

    /* an upload over max_request_size (64K by default) */
    fp = fopen(file, "w");
    CHECK(fp != NULL);
    CHECK_EQ(fwrite(big_body, 1, 100000, fp), 100000);
    fclose(fp);
    out = run("curl -sS -o /dev/null -w '%%{http_code}' --data-binary @%s %s/echo", file, url);
    CHECK(strcmp(out, "413") == 0);

    /* HEAD */
    out = run("curl -sS -I %s/hello", url);
    CHECK(strstr(out, "HTTP/1.1 200 OK\r\n") == out);
    CHECK(strstr(out, "\r\nContent-Length: 6\r\n") != NULL);

    /* two requests in one run reuse the connection */
    out = run("curl -sS -o /dev/null -o /dev/null -w '%%{http_code}:%%{num_connects} ' %s/hello %s/hello", url, url);
    CHECK(strcmp(out, "200:1 200:0 ") == 0);

    /* a large download */
    out = run("curl -sS %s/big | wc -c", url);
    CHECK_EQ(atoi(out), BIG_SIZE);

    /* HTTP/1.0 */
    out = run("curl -sS -0 %s/hello", url);
    CHECK(strcmp(out, "hello\n") == 0);

    /* the unix socket bind */
    out = run("curl -sS --unix-socket %s http://localhost/hello", strchr(unix_cfg, '@') + 1);
    CHECK(strcmp(out, "hello\n") == 0);

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    free(big_body);
    return 0;
}
