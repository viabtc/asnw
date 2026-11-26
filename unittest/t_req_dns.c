/*
 * nw_req and the names of servers. The system resolver is replaced by
 * the getaddrinfo below, so that the test decides what a name means: a
 * name with a dead address before the good one, a name that does not
 * exist, and a lookup that takes longer than the request may.
 */
# include <netdb.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include "common.h"
# include "nw_req.h"
# include "nw_http.h"

# define IP(n) (0x7f000000u + (n))

static int lookups;
static int wrong;
static int port;

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    uint32_t ips[4];
    int count = 0;
    __sync_fetch_and_add(&lookups, 1);
    /* asked for the port of the url, and for tcp only */
    if (atoi(service) != port || hints == NULL || hints->ai_socktype != SOCK_STREAM)
        __sync_fetch_and_add(&wrong, 1);
    if (strcmp(node, "two.test") == 0) {
        ips[count++] = IP(2);           /* nobody there */
        ips[count++] = IP(3);           /* nor there */
        ips[count++] = IP(1);
    } else if (strcmp(node, "dead.test") == 0) {
        ips[count++] = IP(2);
        ips[count++] = IP(3);
    } else if (strcmp(node, "slow.test") == 0) {
        usleep(400 * 1000);
        ips[count++] = IP(1);
    } else {
        return EAI_NONAME;
    }

    struct addrinfo *head = NULL, **link = &head;
    for (int i = 0; i < count; ++i) {
        struct addrinfo *ai = calloc(1, sizeof(struct addrinfo) + sizeof(struct sockaddr_in));
        struct sockaddr_in *in = (struct sockaddr_in *)(ai + 1);
        in->sin_family = AF_INET;
        in->sin_addr.s_addr = htonl(ips[i]);
        in->sin_port = htons(atoi(service));
        ai->ai_family = AF_INET;
        ai->ai_socktype = SOCK_STREAM;
        ai->ai_addrlen = sizeof(struct sockaddr_in);
        ai->ai_addr = (struct sockaddr *)in;
        *link = ai;
        link = &ai->ai_next;
    }
    *res = head;
    return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res);
        res = next;
    }
}

static void on_request(nw_ses *ses, nw_http_request *req)
{
    char body[256];
    char *host = nw_http_get_header(req, "Host");
    int len = snprintf(body, sizeof(body), "%s for %s", req->path, host ? host : "-");
    CHECK_EQ(nw_http_reply_body(ses, 200, "text/plain", body, len), 0);
}

static int calls, last_error, last_status;
static char last_body[256], last_message[256];

static void on_response(int error, nw_req_response *rsp, void *privdata)
{
    calls++;
    last_error = error;
    last_status = rsp->status;
    snprintf(last_body, sizeof(last_body), "%s", rsp->body);
    snprintf(last_message, sizeof(last_message), "%s", rsp->error_msg);
}

static void fetch(const char *host, double timeout)
{
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/config", host, port);
    calls = 0;
    CHECK(nw_req_get(url, timeout, on_response, NULL) != NULL);
    for (int i = 0; i < 3000 && calls == 0; ++i)
        ut_run_for(0.01);
    CHECK_EQ(calls, 1);
    if (last_error)
        fprintf(stderr, "%-14s error %d: %s\n", host, last_error, last_message);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    char text[256];

    static nw_svr_bind bind;
    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1:0", &bind.addr, &bind.sock_type), 0);
    nw_http_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.bind_arr = &bind;
    nw_http_svr_type type;
    memset(&type, 0, sizeof(type));
    type.on_request = on_request;
    nw_http_svr *http = nw_http_svr_create(&cfg, &type, NULL);
    CHECK(http != NULL);
    CHECK_EQ(nw_http_svr_start(http), 0);
    port = ut_svr_port(http->raw_svr, 0);

    /* the addresses of a name are tried in turn, and the server is told
     * the name, not the address */
    fetch("two.test", 5.0);
    CHECK_EQ(last_error, 0);
    CHECK_EQ(last_status, 200);
    snprintf(text, sizeof(text), "/config for two.test:%d", port);
    CHECK(strcmp(last_body, text) == 0);
    CHECK_EQ(lookups, 1);

    /* none of them answers */
    fetch("dead.test", 5.0);
    CHECK_EQ(last_error, NW_REQ_ERR_CONNECT);
    snprintf(text, sizeof(text), "connect to dead.test:%d: ", port);
    CHECK(strncmp(last_message, text, strlen(text)) == 0);

    /* no such name */
    fetch("nowhere.test", 5.0);
    CHECK_EQ(last_error, NW_REQ_ERR_RESOLVE);
    CHECK(strncmp(last_message, "resolve nowhere.test: ", 22) == 0);

    /* a lookup longer than the timeout: no connection is tried after it */
    double begin = ev_time();
    fetch("slow.test", 0.1);
    CHECK_EQ(last_error, NW_REQ_ERR_TIMEOUT);
    CHECK(ev_time() - begin >= 0.35);
    /* and with time to spare it is just a slow request */
    fetch("slow.test", 5.0);
    CHECK_EQ(last_error, 0);
    snprintf(text, sizeof(text), "/config for slow.test:%d", port);
    CHECK(strcmp(last_body, text) == 0);

    /* an address in the url is looked up too, by the real resolver: here
     * it is not a name this test knows */
    fetch("127.0.0.1", 5.0);
    CHECK_EQ(last_error, NW_REQ_ERR_RESOLVE);

    CHECK_EQ(__sync_fetch_and_add(&wrong, 0), 0);
    nw_http_svr_release(http);
    nw_loop_release();
    return 0;
}
