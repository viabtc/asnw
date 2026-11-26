/*
 * A http test client on a plain socket, shared by the nw_http tests.
 * Before including this, define BIG_SIZE (the biggest response a test
 * receives). The client runs the loop of the calling thread while it
 * waits.
 */
# ifndef _UT_HTTP_CLIENT_H_
# define _UT_HTTP_CLIENT_H_

/* a test client: a socket and what was read from it and not used yet */
struct client {
    int fd;
    size_t len;
    bool eof;
    char buf[BIG_SIZE + 65536];
};

struct response {
    int status;
    char head[4096];
    char *body;
    size_t body_size;
};

UT_UNUSED static struct client *client_new(nw_addr *addr)
{
    struct client *c = calloc(1, sizeof(struct client));
    c->fd = socket(addr->family, SOCK_STREAM, 0);
    CHECK(c->fd >= 0);
    CHECK_EQ(connect(c->fd, NW_SOCKADDR(addr), addr->addrlen), 0);
    return c;
}

UT_UNUSED static void client_free(struct client *c)
{
    close(c->fd);
    free(c);
}

UT_UNUSED static void client_send(struct client *c, const char *data, size_t size)
{
    CHECK_EQ(send(c->fd, data, size, MSG_NOSIGNAL), (ssize_t)size);
}

UT_UNUSED static void client_puts(struct client *c, const char *s)
{
    client_send(c, s, strlen(s));
}

/* let the server work, then take what it sent */
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

/* wait for one complete response and take it out of the client's buffer.
 * rsp->body stays valid until the next call */
UT_UNUSED static bool client_read(struct client *c, struct response *rsp, bool no_body, double timeout)
{
    static char *last_body;
    for (double waited = 0; ; waited += 0.002) {
        char *head_end = c->len >= 4 ? strstr(c->buf, "\r\n\r\n") : NULL;
        if (head_end) {
            size_t head_size = head_end + 4 - c->buf;
            size_t body_size = 0;
            char *length = strcasestr(c->buf, "\r\nContent-Length:");
            if (length && length < head_end && !no_body)
                body_size = strtoul(length + 17, NULL, 10);
            if (c->len >= head_size + body_size) {
                CHECK(head_size < sizeof(rsp->head));
                CHECK(sscanf(c->buf, "HTTP/1.1 %d ", &rsp->status) == 1);
                memcpy(rsp->head, c->buf, head_size);
                rsp->head[head_size] = 0;
                free(last_body);
                last_body = malloc(body_size + 1);
                memcpy(last_body, c->buf + head_size, body_size);
                last_body[body_size] = 0;
                rsp->body = last_body;
                rsp->body_size = body_size;
                c->len -= head_size + body_size;
                memmove(c->buf, c->buf + head_size + body_size, c->len + 1);
                return true;
            }
        }
        if (c->eof || waited >= timeout)
            return false;
        client_pump(c, 0.002);
    }
}

/* the server closed the connection and sent nothing more */
UT_UNUSED static bool client_closed(struct client *c, double timeout)
{
    for (double waited = 0; !c->eof && waited < timeout; waited += 0.002)
        client_pump(c, 0.002);
    return c->eof && c->len == 0;
}

UT_UNUSED static int simple(nw_addr *addr, const char *request, struct response *rsp)
{
    struct client *c = client_new(addr);
    client_puts(c, request);
    bool ok = client_read(c, rsp, false, 3.0);
    client_free(c);
    return ok ? rsp->status : -1;
}

# endif
