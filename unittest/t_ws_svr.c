/*
 * nw_ws_svr end to end, driven by plain sockets so that every frame and
 * every packet boundary is under control: the handshake and its refusals,
 * messages of every size class, fragments, control frames, protocol
 * violations, closing from either side, broadcast, sending by id, pings
 * and timeouts.
 */
# include "common.h"
# include "nw_ws.h"

# define BIG_SIZE (300 * 1024)

struct user {
    char path[64];
    char token[32];
    int messages;
};

static nw_ws_svr *svr;
static nw_addr addr;
static char *big;
static int alloc_count, free_count, open_count, close_count, upgrade_count, error_count;
static int message_count;
static size_t last_size;
static bool last_binary;
static int last_broadcast = -1;
static uint64_t saved_id;
static char last_path[64], last_token[32];

static void *on_privdata_alloc(void *s)
{
    CHECK(s == svr);
    alloc_count++;
    return calloc(1, sizeof(struct user));
}

static void on_privdata_free(void *s, void *privdata)
{
    free_count++;
    free(privdata);
}

static int on_upgrade(nw_ses *ses, nw_http_request *req)
{
    upgrade_count++;
    struct user *user = nw_ws_privdata(ses);
    CHECK(user != NULL);
    snprintf(user->path, sizeof(user->path), "%s", req->path);
    const char *token = nw_http_get_header(req, "x-token");
    snprintf(user->token, sizeof(user->token), "%s", token ? token : "");
    /* nothing can be sent before the handshake is answered */
    CHECK_EQ(nw_ws_send_text(ses, "too early"), -1);
    return strcmp(req->path, "/deny") == 0 ? -1 : 0;
}

static void on_open(nw_ses *ses)
{
    open_count++;
    struct user *user = nw_ws_privdata(ses);
    if (strcmp(user->path, "/welcome") == 0)
        CHECK_EQ(nw_ws_send_text(ses, "welcome"), 0);
}

static void on_message(nw_ses *ses, bool binary, void *data, size_t size)
{
    char *s = data;
    struct user *user = nw_ws_privdata(ses);
    CHECK(nw_ws_svr_of(ses) == svr);
    CHECK_EQ(s[size], 0);                       /* always terminated */
    message_count++;
    user->messages++;
    last_size = size;
    last_binary = binary;
    snprintf(last_path, sizeof(last_path), "%s", user->path);
    snprintf(last_token, sizeof(last_token), "%s", user->token);

    if (!binary && strcmp(s, "close") == 0) {
        nw_ws_close(ses, NW_WS_CLOSE_NORMAL, "bye");
        CHECK_EQ(nw_ws_send_text(ses, "after close"), -1);
        nw_ws_close(ses, NW_WS_CLOSE_NORMAL, "twice");      /* no effect */
    } else if (!binary && strcmp(s, "hardclose") == 0) {
        nw_svr_close(svr->raw_svr, ses);
    } else if (!binary && strncmp(s, "broadcast:", 10) == 0) {
        last_broadcast = nw_ws_broadcast_text(svr, s + 10);
    } else if (!binary && strcmp(s, "bigcast") == 0) {
        last_broadcast = nw_ws_broadcast_binary(svr, big, BIG_SIZE);
    } else if (!binary && strcmp(s, "big") == 0) {
        CHECK_EQ(nw_ws_send_binary(ses, big, BIG_SIZE), 0);
    } else if (!binary && strcmp(s, "id") == 0) {
        saved_id = ses->id;
    } else if (binary) {
        CHECK_EQ(nw_ws_send_binary(ses, data, size), 0);
    } else {
        CHECK_EQ(nw_ws_send_text(ses, s), 0);
    }
}

static void on_close(nw_ses *ses)
{
    close_count++;
    CHECK(nw_ws_privdata(ses) != NULL);
    /* the connection is over: sending fails, quietly */
    CHECK_EQ(nw_ws_send_text(ses, "too late"), -1);
    nw_ws_close(ses, NW_WS_CLOSE_NORMAL, NULL);
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    error_count++;
}

# include "ws_client.h"

/* raw bytes right after the handshake must get the connection failed */
static void expect_failure(const uint8_t *bytes, size_t size, int code)
{
    struct client *c = ws_connect("/");
    client_send(c, bytes, size);
    expect_close(c, code);
    client_free(c);
}

static void on_id_timer(nw_timer *timer, void *privdata)
{
    int *result = privdata;
    *result = nw_ws_send_text_id(svr, saved_id, "pushed");
}

static nw_ws_svr *start_server(nw_ws_svr_cfg *cfg, nw_ws_svr_type *type)
{
    nw_ws_svr *s = nw_ws_svr_create(cfg, type, NULL);
    CHECK(s != NULL);
    CHECK_EQ(nw_ws_svr_start(s), 0);
    CHECK_EQ(nw_sock_local_addr(s->raw_svr->binds[0].sockfd, &addr), 0);
    return s;
}

int main(void)
{
    struct client *c;
    struct frame f;
    char head[2048];
    uint8_t raw[256];
    size_t raw_size;

    big = malloc(BIG_SIZE);
    for (int i = 0; i < BIG_SIZE; ++i)
        big[i] = 'A' + i % 26;

    nw_svr_bind bind;
    CHECK_EQ(nw_sock_addr_parse("tcp@127.0.0.1:0", &bind.addr, &bind.sock_type), 0);
    nw_ws_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.binds = &bind;
    cfg.protocol = "chat.v1";

    nw_ws_svr_type type;
    memset(&type, 0, sizeof(type));
    CHECK(nw_ws_svr_create(&cfg, &type, NULL) == NULL);         /* on_message is required */
    type.on_message = on_message;
    type.on_privdata_alloc = on_privdata_alloc;
    CHECK(nw_ws_svr_create(&cfg, &type, NULL) == NULL);         /* alloc needs free */
    type.on_privdata_free = on_privdata_free;
    type.on_upgrade = on_upgrade;
    type.on_open = on_open;
    type.on_close = on_close;
    type.on_error_msg = on_error_msg;
    nw_svr_bind udp;
    CHECK_EQ(nw_sock_addr_parse("udp@127.0.0.1:0", &udp.addr, &udp.sock_type), 0);
    nw_ws_svr_cfg bad_cfg = cfg;
    bad_cfg.binds = &udp;
    CHECK(nw_ws_svr_create(&bad_cfg, &type, NULL) == NULL);     /* stream only */

    svr = start_server(&cfg, &type);

    /*
     * the handshake. the key and the expected answer are the example of
     * RFC 6455, which checks the sha1 and base64 in the server
     */
    c = client_new();
    CHECK_EQ(client_handshake(c, "/welcome?room=1", "X-Token: secret\r\nSec-WebSocket-Protocol: other, chat.v1\r\n",
                head, sizeof(head)), 101);
    CHECK(strncmp(head, "HTTP/1.1 101 Switching Protocols\r\n", 34) == 0);
    CHECK(strstr(head, "\r\nUpgrade: websocket\r\n") != NULL);
    CHECK(strstr(head, "\r\nConnection: Upgrade\r\n") != NULL);
    CHECK(strstr(head, "\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != NULL);
    CHECK(strstr(head, "\r\nSec-WebSocket-Protocol: chat.v1\r\n") != NULL);
    expect_text(c, "welcome");                          /* sent from on_open */
    ws_send_text(c, "hello");
    expect_text(c, "hello");
    CHECK(strcmp(last_path, "/welcome") == 0);          /* kept from the upgrade request */
    CHECK(strcmp(last_token, "secret") == 0);
    CHECK_EQ(open_count, 1);
    client_free(c);
    ut_run_for(0.05);
    CHECK_EQ(close_count, 1);
    CHECK_EQ(alloc_count, 1);
    CHECK_EQ(free_count, 1);

    /* no subprotocol offered: accepted, none confirmed */
    c = client_new();
    CHECK_EQ(client_handshake(c, "/", "", head, sizeof(head)), 101);
    CHECK(strstr(head, "Sec-WebSocket-Protocol") == NULL);
    client_free(c);
    ut_run_for(0.05);

    /* refused upgrades: an http error, then the connection is closed, and
     * the application never sees an open or a close */
    static const struct { const char *request; int status; } refused[] = {
        { "GET / HTTP/1.1\r\nHost: test\r\n\r\n", 426 },                                   /* plain http */
        { "GET / HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
          "Sec-WebSocket-Version: 13\r\n\r\n", 400 },                                      /* no key */
        { "GET / HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
          "Sec-WebSocket-Key: short\r\nSec-WebSocket-Version: 13\r\n\r\n", 400 },
        { "GET / HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 8\r\n\r\n", 426 },
        { "GET / HTTP/1.1\r\nHost: test\r\nUpgrade: h2c\r\nConnection: Upgrade\r\n"
          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", 426 },
        { "GET / HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\nConnection: keep-alive\r\n"
          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", 426 },
        { "POST / HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", 400 },
        { "GET / HTTP/1.0\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", 400 },
        { "GET / HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
          "Sec-WebSocket-Protocol: other\r\n\r\n", 400 },                                  /* wrong subprotocol */
        { "GET / HTTP/1.1\nHost: test\n\r\n\r\n", 400 },                                   /* malformed http */
        { "\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03\r\n\r\n", 400 },                   /* a TLS hello */
    };
    int opens = open_count, closes = close_count, allocs = alloc_count;
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        c = client_new();
        client_send(c, refused[i].request, strlen(refused[i].request));
        int status = client_handshake(c, "/", "", head, sizeof(head));     /* the extra request is ignored */
        if (status != refused[i].status) {
            fprintf(stderr, "refusal %zu: expected %d, got %d\n", i, refused[i].status, status);
            exit(1);
        }
        CHECK(strstr(head, "\r\nConnection: close\r\n") != NULL);
        if (status == 426)
            CHECK(strstr(head, "\r\nSec-WebSocket-Version: 13\r\n") != NULL);
        CHECK(client_closed(c, 3.0));
        client_free(c);
    }
    CHECK_EQ(open_count, opens);
    CHECK_EQ(close_count, closes);
    CHECK_EQ(alloc_count, allocs);
    /* an upgrade request that never ends */
    c = client_new();
    memset(head, 'x', sizeof(head));
    client_send(c, "GET / HTTP/1.1\r\nHost: test\r\nX: ", 31);
    for (int i = 0; i < 5; ++i)
        send(c->fd, head, sizeof(head), MSG_NOSIGNAL | MSG_DONTWAIT);
    CHECK_EQ(client_handshake(c, "/", "", head, sizeof(head)), 431);
    client_free(c);
    /* refused by the application: its privdata is freed right away */
    c = client_new();
    CHECK_EQ(client_handshake(c, "/deny", "", head, sizeof(head)), 403);
    CHECK(client_closed(c, 3.0));
    client_free(c);
    CHECK_EQ(alloc_count, allocs + 1);
    CHECK_EQ(free_count, alloc_count);
    CHECK_EQ(open_count, opens);
    CHECK_EQ(close_count, closes);

    /*
     * messages. every way a length is encoded, text and binary, and the
     * biggest message the server takes (64K by default)
     */
    c = ws_connect("/");
    static const size_t sizes[] = { 0, 1, 125, 126, 127, 1000, 4096, 4097, 65535, 65536 };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        for (int binary = 0; binary <= 1; ++binary) {
            ws_send(c, true, binary ? 2 : 1, big, sizes[i]);
            CHECK(ws_read(c, &f, 3.0));
            CHECK(f.fin);
            CHECK_EQ(f.opcode, binary ? 2 : 1);
            CHECK_EQ(f.size, sizes[i]);
            CHECK(memcmp(f.data, big, sizes[i]) == 0);
            CHECK_EQ(last_size, sizes[i]);
            CHECK_EQ(last_binary, binary);
        }
    }
    /* several frames in one packet: terminating one message must not
     * damage the frame behind it */
    raw_size = make_frame(raw, true, 1, true, "one", 3);
    raw_size += make_frame(raw + raw_size, true, 1, true, "two", 3);
    raw_size += make_frame(raw + raw_size, true, 2, true, "", 0);
    raw_size += make_frame(raw + raw_size, true, 1, true, "three", 5);
    client_send(c, raw, raw_size);
    expect_text(c, "one");
    expect_text(c, "two");
    CHECK(ws_read(c, &f, 3.0));
    CHECK(f.opcode == 2 && f.size == 0);
    expect_text(c, "three");
    /* a frame arriving one byte at a time */
    raw_size = make_frame(raw, true, 1, true, "slowly but surely", 17);
    for (size_t i = 0; i < raw_size; ++i) {
        client_send(c, raw + i, 1);
        ut_run_for(0.001);
    }
    expect_text(c, "slowly but surely");

    /*
     * fragments: a message in several frames is put together, control
     * frames may come in between
     */
    ws_send(c, false, 1, "frag", 4);
    ws_send(c, false, 0, "men", 3);
    ws_send(c, true, 9, "are you there", 13);           /* a ping in the middle */
    ws_send(c, false, 0, "", 0);
    ws_send(c, true, 0, "ted", 3);
    CHECK(ws_read(c, &f, 3.0));
    CHECK_EQ(f.opcode, 10);                             /* the pong carries the ping's payload */
    CHECK_EQ(f.size, 13);
    CHECK(memcmp(f.data, "are you there", 13) == 0);
    expect_text(c, "fragmented");
    CHECK_EQ(last_size, 10);
    /* a binary message of 40000 bytes in 400 pieces */
    for (int i = 0; i < 400; ++i)
        ws_send(c, i == 399, i == 0 ? 2 : 0, big + i * 100, 100);
    CHECK(ws_read(c, &f, 3.0));
    CHECK_EQ(f.opcode, 2);
    CHECK_EQ(f.size, 40000);
    CHECK(memcmp(f.data, big, 40000) == 0);
    /* control frames: an unsolicited pong is ignored, an empty ping answered */
    ws_send(c, true, 10, "unasked", 7);
    ws_send(c, true, 9, NULL, 0);
    CHECK(ws_read(c, &f, 3.0));
    CHECK(f.opcode == 10 && f.size == 0);
    /* a big message from the server: head and payload written separately */
    ws_send_text(c, "big");
    CHECK(ws_read(c, &f, 5.0));
    CHECK_EQ(f.opcode, 2);
    CHECK_EQ(f.size, BIG_SIZE);
    CHECK(memcmp(f.data, big, BIG_SIZE) == 0);

    /* the client closes: the server answers with the same code and hangs up */
    closes = close_count;
    ws_send(c, true, 8, "\x03\xe9" "going", 7);         /* 1001 */
    expect_close(c, 1001);
    client_free(c);
    ut_run_for(0.05);
    CHECK_EQ(close_count, closes + 1);
    c = ws_connect("/");
    ws_send(c, true, 8, NULL, 0);                       /* a close without a code */
    expect_close(c, 1000);
    client_free(c);

    /* the server closes: close frame with code and reason, the client
     * answers, the server hangs up */
    c = ws_connect("/");
    ws_send_text(c, "close");
    CHECK(ws_read(c, &f, 3.0));
    CHECK_EQ(f.opcode, 8);
    CHECK_EQ(f.size, 5);
    CHECK(memcmp(f.data, "\x03\xe8" "bye", 5) == 0);
    ws_send_text(c, "ignored after close");
    ws_send(c, true, 8, "\x03\xe8", 2);
    CHECK(client_closed(c, 3.0));
    client_free(c);
    /* the application drops the connection without ceremony */
    c = ws_connect("/");
    ws_send_text(c, "hardclose");
    CHECK(client_closed(c, 3.0));
    client_free(c);

    /*
     * protocol violations: close frame 1002 (or 1009 for size), then EOF
     */
    raw_size = make_frame(raw, true, 1, false, "unmasked", 8);
    expect_failure(raw, raw_size, 1002);                /* a client has to mask */
    raw_size = make_frame(raw, true, 1, true, "rsv", 3);
    raw[0] |= 0x40;
    expect_failure(raw, raw_size, 1002);                /* reserved bit */
    raw_size = make_frame(raw, true, 3, true, "x", 1);
    expect_failure(raw, raw_size, 1002);                /* reserved opcode */
    raw_size = make_frame(raw, true, 11, true, "x", 1);
    expect_failure(raw, raw_size, 1002);
    raw_size = make_frame(raw, false, 9, true, "x", 1);
    expect_failure(raw, raw_size, 1002);                /* a fragmented ping */
    raw_size = make_frame(raw, true, 9, true, big, 126);
    expect_failure(raw, raw_size, 1002);                /* a ping over 125 bytes */
    raw_size = make_frame(raw, true, 0, true, "x", 1);
    expect_failure(raw, raw_size, 1002);                /* a continuation of nothing */
    raw_size = make_frame(raw, false, 1, true, "a", 1);
    raw_size += make_frame(raw + raw_size, true, 1, true, "b", 1);
    expect_failure(raw, raw_size, 1002);                /* a new message inside a message */
    raw_size = make_frame(raw, true, 8, true, "x", 1);
    expect_failure(raw, raw_size, 1002);                /* a close code of one byte */
    /* sizes. the length is checked before anything is done with it, the
     * payload does not have to arrive */
    static const uint8_t too_big[] = { 0x82, 0xff, 0, 0, 0, 0, 0, 0x01, 0x00, 0x01 };       /* 65537 */
    expect_failure(too_big, sizeof(too_big), 1009);
    static const uint8_t huge[] = { 0x82, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf0, 1, 2, 3, 4, 5, 6 };
    expect_failure(huge, sizeof(huge), 1009);           /* a length that wraps around when added to */
    static const uint8_t huge2[] = { 0x82, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4 };
    expect_failure(huge2, sizeof(huge2), 1009);
    /* fragments that add up to more than the limit */
    c = ws_connect("/");
    ws_send(c, false, 2, big, 40000);
    ws_send(c, true, 0, big, 40000);
    expect_close(c, 1009);
    client_free(c);
    CHECK(error_count > 10);

    /*
     * robustness: streams of valid frames with bytes flipped, cut short or
     * followed by noise. nothing is asserted about the outcome except that
     * the server survives it (AddressSanitizer is watching) and keeps
     * serving afterwards
     */
    uint32_t seed = 4242;
    for (int iter = 0; iter < 400; ++iter) {
        uint8_t stream[2048];
        size_t stream_size = 0;
        int frames = 1 + iter % 4;
        for (int i = 0; i < frames; ++i) {
            seed = seed * 1103515245 + 12345;
            static const int opcodes[] = { 1, 2, 0, 9, 10, 8, 1, 2 };
            int opcode = opcodes[(seed >> 8) % 8];
            size_t size = (seed >> 12) % 300;
            if (opcode >= 8 && size > 125)
                size %= 126;
            stream_size += make_frame(stream + stream_size, (seed >> 20) % 4 != 0, opcode, true, big, size);
        }
        int flips = iter % 5;
        for (int i = 0; i < flips; ++i) {
            seed = seed * 1103515245 + 12345;
            stream[(seed >> 8) % stream_size] ^= 1 << ((seed >> 20) % 8);
        }
        seed = seed * 1103515245 + 12345;
        if (iter % 3 == 0)
            stream_size = 1 + (seed >> 8) % stream_size;
        c = ws_connect("/");
        client_send(c, stream, stream_size);
        client_pump(c, 0.003);
        client_free(c);
    }
    ut_run_for(0.1);
    c = ws_connect("/");
    ws_send_text(c, "still serving");
    expect_text(c, "still serving");
    client_free(c);
    ut_run_for(0.05);

    /*
     * broadcast: everybody who finished the handshake, nobody else
     */
    struct client *members[5];
    for (int i = 0; i < 5; ++i)
        members[i] = ws_connect("/");
    struct client *stranger = client_new();             /* connected, no handshake */
    client_pump(stranger, 0.05);
    ws_send_text(members[0], "broadcast:news");
    for (int i = 0; i < 5; ++i)
        expect_text(members[i], "news");
    CHECK_EQ(last_broadcast, 5);
    ws_send_text(members[1], "bigcast");
    for (int i = 0; i < 5; ++i) {
        CHECK(ws_read(members[i], &f, 5.0));
        CHECK_EQ(f.size, BIG_SIZE);
        CHECK(memcmp(f.data, big, BIG_SIZE) == 0);
    }
    CHECK_EQ(last_broadcast, 5);
    client_pump(stranger, 0.05);
    CHECK_EQ(stranger->len, 0);
    client_free(stranger);

    /* pushing to a connection by id, from outside its callbacks */
    int push_result = -99;
    nw_timer id_timer;
    ws_send_text(members[2], "id");
    client_pump(members[2], 0.05);
    nw_timer_set(&id_timer, 0.02, false, on_id_timer, &push_result);
    nw_timer_start(&id_timer);
    expect_text(members[2], "pushed");
    CHECK_EQ(push_result, 0);
    client_free(members[2]);
    ut_run_for(0.05);
    nw_timer_set(&id_timer, 0.02, false, on_id_timer, &push_result);
    nw_timer_start(&id_timer);
    ut_run_for(0.1);
    CHECK_EQ(push_result, -1);                          /* that client is gone */

    /* release with clients connected: every one of them gets its on_close */
    closes = close_count;
    nw_ws_svr_release(svr);
    CHECK_EQ(close_count, closes + 4);
    CHECK_EQ(free_count, alloc_count);
    CHECK_EQ(open_count, close_count);
    for (int i = 0; i < 5; ++i) {
        if (i != 2)
            client_free(members[i]);
    }

    /*
     * the origin check: a host name allows that host and its subdomains
     */
    cfg.protocol = NULL;
    cfg.origin = "example.com";
    svr = start_server(&cfg, &type);
    static const struct { const char *origin; int status; } origins[] = {
        { "Origin: https://example.com\r\n", 101 },
        { "Origin: http://EXAMPLE.com:8080\r\n", 101 },
        { "Origin: https://app.example.com\r\n", 101 },
        { "Origin: https://evilexample.com\r\n", 403 },         /* a suffix, but another domain */
        { "Origin: https://example.com.evil.org\r\n", 403 },
        { "Origin: https://example.org\r\n", 403 },
        { "Origin: example.com\r\n", 403 },
        { "", 403 },                                            /* no Origin at all */
    };
    for (size_t i = 0; i < sizeof(origins) / sizeof(origins[0]); ++i) {
        c = client_new();
        int status = client_handshake(c, "/", origins[i].origin, head, sizeof(head));
        if (status != origins[i].status) {
            fprintf(stderr, "origin %zu: expected %d, got %d\n", i, origins[i].status, status);
            exit(1);
        }
        client_free(c);
    }
    nw_ws_svr_release(svr);
    /* a full origin has to match exactly */
    cfg.origin = "https://app.example.com";
    svr = start_server(&cfg, &type);
    c = client_new();
    CHECK_EQ(client_handshake(c, "/", "Origin: https://app.example.com\r\n", head, sizeof(head)), 101);
    client_free(c);
    c = client_new();
    CHECK_EQ(client_handshake(c, "/", "Origin: http://app.example.com\r\n", head, sizeof(head)), 403);
    client_free(c);
    c = client_new();
    CHECK_EQ(client_handshake(c, "/", "Origin: https://x.app.example.com\r\n", head, sizeof(head)), 403);
    client_free(c);
    nw_ws_svr_release(svr);

    /*
     * pings and the idle timeout. one client answers the pings and stays,
     * one stays silent and is closed with 1001
     */
    cfg.origin = NULL;
    cfg.idle_timeout = 2.0;
    cfg.ping_interval = 0.5;
    svr = start_server(&cfg, &type);
    struct client *alive = ws_connect("/");
    struct client *silent = ws_connect("/");
    int pings = 0, silent_pings = 0;
    bool silent_closed = false;
    for (double waited = 0; waited < 4.5; waited += 0.05) {
        client_pump(alive, 0.05);
        while (ws_read(alive, &f, 0)) {
            CHECK_EQ(f.opcode, 9);
            pings++;
            ws_send(alive, true, 10, NULL, 0);
        }
        client_pump(silent, 0);
        while (!silent_closed && ws_read(silent, &f, 0)) {
            if (f.opcode == 9) {
                silent_pings++;
            } else {
                CHECK_EQ(f.opcode, 8);
                CHECK_EQ((f.data[0] << 8) | f.data[1], 1001);
                silent_closed = true;
            }
        }
    }
    fprintf(stderr, "4.5s: %d pings answered, %d ignored before the silent client was closed\n", pings, silent_pings);
    CHECK(pings >= 3);
    CHECK(!alive->eof);
    CHECK(silent_pings >= 1);
    CHECK(silent_closed);
    CHECK(client_closed(silent, 3.0));
    ws_send_text(alive, "still here");
    expect_text(alive, "still here");
    client_free(alive);
    client_free(silent);
    ut_run_for(0.05);
    CHECK_EQ(svr->raw_svr->ses_count, 0);
    nw_ws_svr_release(svr);

    CHECK_EQ(free_count, alloc_count);
    CHECK_EQ(open_count, close_count);
    free(big);
    return 0;
}
