/*
 * A websocket test client on a plain socket, shared by the nw_ws tests.
 * Before including this, define BIG_SIZE (the biggest message a test
 * receives) and a `static nw_addr addr` with the address of the server.
 * The client runs the loop of the calling thread while it waits.
 */
# ifndef _UT_WS_CLIENT_H_
# define _UT_WS_CLIENT_H_


/* a test client: a socket and what was read from it and not used yet */
struct client {
    int fd;
    size_t len;
    bool eof;
    uint8_t buf[BIG_SIZE + 65536];
};

struct frame {
    bool fin;
    int opcode;
    size_t size;
    uint8_t *data;
};

UT_UNUSED static struct client *client_new(void)
{
    struct client *c = calloc(1, sizeof(struct client));
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(c->fd >= 0);
    CHECK_EQ(connect(c->fd, NW_SOCKADDR(&addr), sizeof(addr.in)), 0);
    return c;
}

UT_UNUSED static void client_free(struct client *c)
{
    close(c->fd);
    free(c);
}

UT_UNUSED static void client_send(struct client *c, const void *data, size_t size)
{
    CHECK_EQ(send(c->fd, data, size, MSG_NOSIGNAL), (ssize_t)size);
}

UT_UNUSED static void client_pump(struct client *c, double seconds)
{
    ut_run_for(seconds);
    for (;;) {
        ssize_t n = recv(c->fd, c->buf + c->len, sizeof(c->buf) - c->len - 1, MSG_DONTWAIT);
        if (n > 0) {
            c->len += n;
        } else {
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                c->eof = true;
            break;
        }
    }
    c->buf[c->len] = 0;
}

UT_UNUSED static void client_consume(struct client *c, size_t size)
{
    c->len -= size;
    memmove(c->buf, c->buf + size, c->len + 1);
}

/* send an upgrade request, return the http status of the answer and leave
 * its head in `head` */
UT_UNUSED static int client_handshake(struct client *c, const char *path, const char *headers, char *head, size_t head_size)
{
    char request[10240];
    int len = snprintf(request, sizeof(request),
            "GET %s HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n%s\r\n", path, headers);
    client_send(c, request, len);
    for (double waited = 0; ; waited += 0.002) {
        char *end = strstr((char *)c->buf, "\r\n\r\n");
        if (end) {
            size_t size = end + 4 - (char *)c->buf;
            int status = 0;
            CHECK(sscanf((char *)c->buf, "HTTP/1.1 %d ", &status) == 1);
            CHECK(size < head_size);
            memcpy(head, c->buf, size);
            head[size] = 0;
            if (status != 101) {
                /* an error comes with a short body */
                char *length = strstr(head, "Content-Length: ");
                CHECK(length != NULL);
                size_t body = atoi(length + 16);
                while (c->len < size + body && !c->eof)
                    client_pump(c, 0.002);
                size += body;
            }
            client_consume(c, size);
            return status;
        }
        if (c->eof || waited >= 3.0)
            return -1;
        client_pump(c, 0.002);
    }
}

/* a client that completed the handshake */
UT_UNUSED static struct client *ws_connect(const char *path)
{
    char head[1024];
    struct client *c = client_new();
    CHECK_EQ(client_handshake(c, path, "", head, sizeof(head)), 101);
    return c;
}

/* build a client frame at out, return its size */
UT_UNUSED static size_t make_frame(uint8_t *out, bool fin, int opcode, bool masked, const void *data, size_t size)
{
    static uint32_t seed = 99;
    size_t pos = 0;
    out[pos++] = (fin ? 0x80 : 0) | opcode;
    uint8_t mask_bit = masked ? 0x80 : 0;
    if (size < 126) {
        out[pos++] = mask_bit | size;
    } else if (size <= 0xffff) {
        out[pos++] = mask_bit | 126;
        out[pos++] = size >> 8;
        out[pos++] = size;
    } else {
        out[pos++] = mask_bit | 127;
        for (int i = 7; i >= 0; --i)
            out[pos++] = (uint64_t)size >> (i * 8);
    }
    uint8_t mask[4] = { 0, 0, 0, 0 };
    if (masked) {
        for (int i = 0; i < 4; ++i) {
            seed = seed * 1103515245 + 12345;
            mask[i] = seed >> 16;
            out[pos++] = mask[i];
        }
    }
    for (size_t i = 0; i < size; ++i)
        out[pos++] = ((const uint8_t *)data)[i] ^ mask[i & 3];
    return pos;
}

UT_UNUSED static void ws_send(struct client *c, bool fin, int opcode, const void *data, size_t size)
{
    uint8_t *frame = malloc(size + 16);
    size_t frame_size = make_frame(frame, fin, opcode, true, data, size);
    client_send(c, frame, frame_size);
    free(frame);
}

UT_UNUSED static void ws_send_text(struct client *c, const char *text)
{
    ws_send(c, true, 1, text, strlen(text));
}

/* wait for one frame from the server. frame->data stays valid until the
 * next call */
UT_UNUSED static bool ws_read(struct client *c, struct frame *frame, double timeout)
{
    static uint8_t *last;
    for (double waited = 0; ; waited += 0.002) {
        if (c->len >= 2) {
            size_t header = 2;
            uint64_t size = c->buf[1] & 0x7f;
            CHECK_EQ(c->buf[1] & 0x80, 0);              /* a server never masks */
            CHECK_EQ(c->buf[0] & 0x70, 0);
            bool ready = true;
            if (size == 126) {
                header = 4;
                ready = c->len >= 4;
                if (ready)
                    size = (c->buf[2] << 8) | c->buf[3];
            } else if (size == 127) {
                header = 10;
                ready = c->len >= 10;
                size = 0;
                for (int i = 0; ready && i < 8; ++i)
                    size = (size << 8) | c->buf[2 + i];
            }
            if (ready && c->len >= header + size) {
                frame->fin = (c->buf[0] & 0x80) != 0;
                frame->opcode = c->buf[0] & 0x0f;
                frame->size = size;
                free(last);
                last = malloc(size + 1);
                memcpy(last, c->buf + header, size);
                last[size] = 0;
                frame->data = last;
                client_consume(c, header + size);
                return true;
            }
        }
        if (c->eof || waited >= timeout)
            return false;
        client_pump(c, 0.002);
    }
}

UT_UNUSED static bool client_closed(struct client *c, double timeout)
{
    for (double waited = 0; !c->eof && waited < timeout; waited += 0.002)
        client_pump(c, 0.002);
    return c->eof && c->len == 0;
}

/* the next frame is a text frame with exactly this content */
UT_UNUSED static void expect_text(struct client *c, const char *text)
{
    struct frame f;
    CHECK(ws_read(c, &f, 3.0));
    CHECK(f.fin);
    CHECK_EQ(f.opcode, 1);
    CHECK_EQ(f.size, strlen(text));
    CHECK(memcmp(f.data, text, f.size) == 0);
}

/* the server fails the connection: a close frame with this code, then EOF */
UT_UNUSED static void expect_close(struct client *c, int code)
{
    struct frame f;
    CHECK(ws_read(c, &f, 3.0));
    CHECK_EQ(f.opcode, 8);
    CHECK(f.size >= 2);
    int got = (f.data[0] << 8) | f.data[1];
    if (got != code) {
        fprintf(stderr, "expected close code %d, got %d\n", code, got);
        exit(1);
    }
    CHECK(client_closed(c, 3.0));
}

# endif
