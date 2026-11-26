/*
 * nw_clt with a server given by name: lookup before connecting, the
 * addresses of a name tried one after the other as one attempt, a new
 * lookup after a lost connection, names that fail, and closing or
 * releasing the client while a lookup is running.
 *
 * The system resolver is replaced by the getaddrinfo below, so that the
 * test decides what each name means: 127.0.0.1, .2, .3 ... are all the
 * local machine, but only the ones a server listens on answer.
 */
# include <netdb.h>
# include <sys/socket.h>
# include "common.h"
# include "nw_dns.h"

# define IP(n) (0x7f000000u + (n))

/* what the names mean. read on the resolver threads */
static int lookups;                     /* all names together */
static uint32_t moving_ip = IP(1);

# define ADD(var) __sync_fetch_and_add(&(var), 1)
# define GET(var) __sync_fetch_and_add(&(var), 0)

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    uint32_t ips[8];
    int count = 0;
    ADD(lookups);
    if (strcmp(node, "one.test") == 0) {
        ips[count++] = IP(1);
    } else if (strcmp(node, "two.test") == 0) {
        ips[count++] = IP(2);           /* nobody there */
        ips[count++] = IP(1);
    } else if (strcmp(node, "dead.test") == 0) {
        ips[count++] = IP(2);
        ips[count++] = IP(4);
        ips[count++] = IP(5);
    } else if (strcmp(node, "moving.test") == 0) {
        ips[count++] = __sync_fetch_and_add(&moving_ip, 0);
    } else if (strcmp(node, "slow.test") == 0) {
        usleep(300 * 1000);
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

/* the servers: an echo on 127.0.0.1 and one on 127.0.0.3, same port */
static nw_svr *svr1, *svr3;
static int port;

static void svr_on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    nw_ses_send(ses, data, size);
}

static void drop_clients(nw_svr *svr)
{
    nw_svr_iterator iter;
    nw_ses *ses;
    nw_svr_iter_init(&iter, svr);
    while ((ses = nw_svr_next(&iter)) != NULL)
        nw_svr_close_clt(svr, ses);
}

/* a client and what happened to it */
struct client {
    nw_clt *clt;
    int connected, failed, closed, errors, received;
    uint32_t peer_ip;
    char last_error[256];
};

static void on_connect(nw_ses *ses, bool result)
{
    struct client *c = ses->privdata;
    if (result) {
        c->connected++;
        c->peer_ip = ntohl(ses->peer_addr.in.sin_addr.s_addr);
        CHECK_EQ(ntohs(ses->peer_addr.in.sin_port), port);
    } else {
        c->failed++;
    }
}

static int on_close(nw_ses *ses)
{
    struct client *c = ses->privdata;
    c->closed++;
    return 0;
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    struct client *c = ses->privdata;
    c->errors++;
    snprintf(c->last_error, sizeof(c->last_error), "%s", msg);
}

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    struct client *c = ses->privdata;
    c->received++;
}

static void client_start(struct client *c, const char *proto, const char *host, double reconnect)
{
    char cfg[128];
    snprintf(cfg, sizeof(cfg), "%s@%s:%d", proto, host, port);
    nw_clt_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = ut_line_decode;
    type.on_recv_pkg = on_recv_pkg;
    type.on_connect = on_connect;
    type.on_close = on_close;
    type.on_error_msg = on_error_msg;
    memset(c, 0, sizeof(*c));
    c->clt = ut_clt(cfg, &type, 1024, reconnect, c);
    CHECK_EQ(nw_clt_start(c->clt), 0);
}

# define WAIT(cond) do { \
    for (int _i = 0; _i < 1000 && !(cond); ++_i) \
        ut_run_for(0.01); \
    CHECK(cond); \
} while (0)

static void check_parse(const char *str, int ret, const char *host, int cfg_port)
{
    nw_clt_cfg cfg;
    memset(&cfg, 0xff, sizeof(cfg));
    int got = nw_clt_cfg_parse(str, &cfg);
    if (got != ret) {
        fprintf(stderr, "nw_clt_cfg_parse(\"%s\") = %d, expected %d\n", str, got, ret);
        exit(1);
    }
    if (ret == 0) {
        CHECK(strcmp(cfg.name, host) == 0);
        CHECK_EQ(cfg.port, cfg_port);
    }
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    struct client c;

    /* what is a name and what is not */
    check_parse("tcp@db.local:5432", 0, "db.local", 5432);
    check_parse("udp@stats-1.internal:8125", 0, "stats-1.internal", 8125);
    check_parse("TCP@localhost:80", 0, "localhost", 80);
    check_parse("tcp@a_b.c:1", 0, "a_b.c", 1);
    check_parse("tcp@10.0.0.1:5432", 0, "", 0);                 /* an address: no name */
    check_parse("tcp@[::1]:5432", 0, "", 0);
    check_parse("stream@/tmp/db.sock", 0, "", 0);
    check_parse("tcp@db.local", -3, NULL, 0);                   /* no port */
    check_parse("tcp@db.local:port", -4, NULL, 0);
    check_parse("tcp@db.local:70000", -4, NULL, 0);
    check_parse("tcp@300.1.1.1:80", -4, NULL, 0);               /* a bad ip stays a bad ip */
    check_parse("tcp@1.2.3:80", -4, NULL, 0);
    check_parse("tcp@fe80::zz:80", -4, NULL, 0);
    check_parse("tcp@db local:80", -6, NULL, 0);
    check_parse("tcp@db/local:80", -6, NULL, 0);
    check_parse("tcp@:80", -6, NULL, 0);
    check_parse("http@db.local:80", -2, NULL, 0);
    /* names are for tcp and udp */
    nw_clt_cfg bad;
    memset(&bad, 0, sizeof(bad));
    snprintf(bad.name, sizeof(bad.name), "one.test");
    bad.sock_type = SOCK_SEQPACKET;
    bad.max_pkg_size = 1024;
    nw_clt_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = ut_line_decode;
    type.on_recv_pkg = on_recv_pkg;
    CHECK(nw_clt_create(&bad, &type, NULL) == NULL);

    nw_svr_type st;
    memset(&st, 0, sizeof(st));
    st.decode_pkg = ut_line_decode;
    st.on_recv_pkg = svr_on_recv_pkg;
    svr1 = ut_svr("tcp@127.0.0.1:0", &st, 1024, NULL);
    port = ut_svr_port(svr1, 0);
    char cfg3[64];
    snprintf(cfg3, sizeof(cfg3), "tcp@127.0.0.3:%d", port);
    svr3 = ut_svr(cfg3, &st, 1024, NULL);

    /* an address needs no lookup */
    client_start(&c, "tcp", "127.0.0.1", 1.0);
    WAIT(c.connected == 1);
    CHECK_EQ(GET(lookups), 0);
    nw_clt_release(c.clt);

    /*
     * a name with two addresses, the first one dead. the client gets to
     * the second without the application hearing of the first, and long
     * before a reconnect_timeout has passed
     */
    client_start(&c, "tcp", "two.test", 5.0);
    CHECK(!nw_clt_connected(c.clt));                    /* looking the name up */
    double begin = ev_time();
    WAIT(c.connected == 1);
    CHECK(ev_time() - begin < 2.0);
    CHECK_EQ(c.peer_ip, IP(1));
    CHECK_EQ(c.failed, 0);
    CHECK_EQ(c.closed, 0);
    CHECK_EQ(c.errors, 0);
    CHECK_EQ(GET(lookups), 1);
    CHECK_EQ(nw_ses_send(&c.clt->ses, "hello\n", 6), 0);
    WAIT(c.received == 1);
    nw_clt_release(c.clt);

    /* the connection is lost: the name is looked up again, and the client
     * goes where it points now */
    lookups = 0;
    client_start(&c, "tcp", "moving.test", 0.1);
    WAIT(c.connected == 1);
    CHECK_EQ(c.peer_ip, IP(1));
    CHECK_EQ(svr1->clt_count, 1);
    CHECK_EQ(GET(lookups), 1);
    __sync_lock_test_and_set(&moving_ip, IP(3));
    drop_clients(svr1);
    WAIT(c.connected == 2);
    CHECK_EQ(c.closed, 1);
    CHECK_EQ(c.peer_ip, IP(3));
    CHECK_EQ(svr3->clt_count, 1);
    CHECK_EQ(svr1->clt_count, 0);
    CHECK_EQ(GET(lookups), 2);
    CHECK_EQ(nw_ses_send(&c.clt->ses, "there\n", 6), 0);
    WAIT(c.received == 1);
    /* closed by the application and started again: looked up again too */
    __sync_lock_test_and_set(&moving_ip, IP(1));
    CHECK_EQ(nw_clt_close(c.clt), 0);
    CHECK_EQ(c.closed, 2);
    CHECK_EQ(nw_clt_start(c.clt), 0);
    WAIT(c.connected == 3);
    CHECK_EQ(c.peer_ip, IP(1));
    CHECK_EQ(GET(lookups), 3);
    nw_clt_release(c.clt);

    /* every address dead: one failed attempt per round, not one per
     * address, and a new lookup for each round */
    lookups = 0;
    client_start(&c, "tcp", "dead.test", 0.1);
    WAIT(c.failed >= 3);
    int rounds = c.failed;
    CHECK(c.closed == rounds || c.closed == rounds - 1);
    CHECK(GET(lookups) >= rounds && GET(lookups) <= rounds + 1);
    CHECK_EQ(c.connected, 0);
    CHECK_EQ(c.errors, 0);
    CHECK(!nw_clt_connected(c.clt));
    nw_clt_release(c.clt);

    /* a name that does not resolve: reported, and tried again */
    client_start(&c, "tcp", "nowhere.test", 0.1);
    WAIT(c.closed >= 3);
    CHECK(c.errors >= 3);
    CHECK(strncmp(c.last_error, "resolve nowhere.test: ", 22) == 0);
    fprintf(stderr, "a name that does not resolve: %s\n", c.last_error);
    CHECK_EQ(c.connected, 0);
    CHECK_EQ(c.failed, 0);
    CHECK(!nw_clt_connected(c.clt));
    nw_clt_release(c.clt);

    /* closed while the lookup is running: its answer changes nothing */
    client_start(&c, "tcp", "slow.test", 0.1);
    ut_run_for(0.05);
    CHECK(c.clt->dns_query != NULL);
    CHECK_EQ(nw_clt_close(c.clt), 0);
    CHECK(c.clt->dns_query == NULL);
    ut_run_for(0.5);
    CHECK_EQ(c.connected, 0);
    CHECK(!nw_clt_connected(c.clt));
    /* started twice in a row: one connection */
    CHECK_EQ(nw_clt_start(c.clt), 0);
    CHECK_EQ(nw_clt_start(c.clt), 0);
    WAIT(c.connected == 1);
    ut_run_for(0.4);
    CHECK_EQ(c.connected, 1);
    nw_clt_release(c.clt);
    /* released while the lookup is running */
    client_start(&c, "tcp", "slow.test", 0.1);
    ut_run_for(0.05);
    nw_clt_release(c.clt);
    ut_run_for(0.5);

    /* udp by name: on_connect tells when the client can send */
    nw_svr *udp = NULL;
    char udp_cfg[64];
    snprintf(udp_cfg, sizeof(udp_cfg), "udp@127.0.0.1:%d", port);
    udp = ut_svr(udp_cfg, &st, 1024, NULL);
    client_start(&c, "udp", "one.test", 0.1);
    CHECK(!nw_clt_connected(c.clt));
    WAIT(c.connected == 1);
    CHECK(nw_clt_connected(c.clt));
    CHECK_EQ(c.peer_ip, IP(1));
    CHECK_EQ(nw_ses_send(&c.clt->ses, "datagram\n", 9), 0);
    WAIT(c.received == 1);
    nw_clt_release(c.clt);
    /* and a udp name that does not resolve is tried again as well */
    client_start(&c, "udp", "nowhere.test", 0.1);
    WAIT(c.errors >= 3);
    CHECK_EQ(c.connected, 0);
    nw_clt_release(c.clt);

    nw_svr_release(udp);
    nw_svr_release(svr1);
    nw_svr_release(svr3);
    nw_loop_release();
    return 0;
}
