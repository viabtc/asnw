/*
 * Description: a small http client: GET or POST without blocking the loop
 *     History: yang@haipo.me, 2026/11/26, create
 */

# include <stdio.h>
# include <stdlib.h>
# include <stdarg.h>
# include <stdbool.h>
# include <string.h>
# include <strings.h>
# include <ctype.h>
# include <errno.h>
# include <time.h>
# include <poll.h>
# include <netdb.h>
# include <unistd.h>
# include <sys/socket.h>

# include "nw_req.h"
# include "nw_job.h"
# include "nw_evt.h"

# define HEAD_MAX       (64 * 1024)
# define CHUNK_LINE_MAX 1024

struct nw_req {
    /* the request: written by nw_req_get, read by the worker */
    char *host;
    char port[8];
    char *text;
    size_t text_size;
    double deadline;
    nw_req_callback callback;
    void *privdata;
    /* the two ways a worker is told to give up, read with atomics: set by
     * nw_req_cancel, and set for all requests when the loop is released */
    int cancelled;
    int *stopping;

    /* the result: written by the worker before it hands the request back */
    int error;
    char error_msg[256];
    char *data;
    size_t len;
    size_t cap;
    size_t head_size;
    size_t body_size;
    int status;
    uint32_t header_count;
    struct {
        uint32_t name;
        uint32_t value;
    } offsets[NW_REQ_MAX_HEADERS];

    nw_req_response rsp;
};

/* the request threads of the calling loop thread */
static __thread nw_job *pool;
static __thread int stopping;

static double monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int fail(struct nw_req *req, int error, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int fail(struct nw_req *req, int error, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req->error_msg, sizeof(req->error_msg), fmt, ap);
    va_end(ap);
    req->error = error;
    return -1;
}

static bool given_up(struct nw_req *req)
{
    return __atomic_load_n(&req->cancelled, __ATOMIC_RELAXED) || __atomic_load_n(req->stopping, __ATOMIC_RELAXED);
}

/*
 * wait until fd is ready or the time is up. 1: ready, 0: time is up, -1:
 * poll failed. the wait is cut in short pieces to notice a request that
 * nobody waits for any more; that one is reported as out of time, to
 * nobody.
 */
static int wait_fd(struct nw_req *req, int fd, short events, double until)
{
    for (;;) {
        if (given_up(req))
            return 0;
        double left = until - monotonic();
        if (left <= 0)
            return 0;
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = events;
        pfd.revents = 0;
        int ret = poll(&pfd, 1, left > 0.1 ? 100 : (int)(left * 1000) + 1);
        if (ret > 0)
            return 1;
        if (ret < 0 && errno != EINTR)
            return -1;
    }
}

/* look the name up and connect to the first of its addresses that takes
 * the connection. return the socket, or -1 */
static int open_connection(struct nw_req *req)
{
    struct addrinfo hints;
    struct addrinfo *list = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int ret = getaddrinfo(req->host, req->port, &hints, &list);
    if (ret == EAI_SYSTEM)
        return fail(req, NW_REQ_ERR_RESOLVE, "resolve %s: %m", req->host);
    if (ret != 0)
        return fail(req, NW_REQ_ERR_RESOLVE, "resolve %s: %s", req->host, gai_strerror(ret));

    int left_to_try = 0;
    for (struct addrinfo *ai = list; ai; ai = ai->ai_next)
        left_to_try++;

    int fd = -1;
    int why = ETIMEDOUT;
    for (struct addrinfo *ai = list; ai; ai = ai->ai_next, left_to_try--) {
        double now = monotonic();
        if (now >= req->deadline || given_up(req)) {
            why = ETIMEDOUT;
            break;
        }
        fd = socket(ai->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            why = errno;
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        why = errno;
        if (why == EINPROGRESS) {
            /* an address that does not answer must not use up the time of
             * the ones after it: they share what is left */
            ret = wait_fd(req, fd, POLLOUT, now + (req->deadline - now) / left_to_try);
            if (ret > 0) {
                socklen_t len = sizeof(why);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &why, &len) < 0)
                    why = errno;
                if (why == 0)
                    break;
            } else {
                why = ret == 0 ? ETIMEDOUT : errno;
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);

    if (fd < 0) {
        if (why == ETIMEDOUT)
            return fail(req, NW_REQ_ERR_TIMEOUT, "timeout connecting to %s:%s", req->host, req->port);
        errno = why;
        return fail(req, NW_REQ_ERR_CONNECT, "connect to %s:%s: %m", req->host, req->port);
    }
    return fd;
}

static int send_request(struct nw_req *req, int fd)
{
    size_t sent = 0;
    while (sent < req->text_size) {
        ssize_t n = send(fd, req->text + sent, req->text_size - sent, MSG_NOSIGNAL);
        if (n >= 0) {
            sent += n;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            int ret = wait_fd(req, fd, POLLOUT, req->deadline);
            if (ret == 0)
                return fail(req, NW_REQ_ERR_TIMEOUT, "timeout sending the request");
            if (ret < 0)
                return fail(req, NW_REQ_ERR_IO, "poll: %m");
        } else if (errno != EINTR) {
            return fail(req, NW_REQ_ERR_IO, "send: %m");
        }
    }
    return 0;
}

/* read some more of the response. > 0: that many bytes, 0: the server
 * closed the connection, -1: failed. one byte of data is always left free,
 * for the '\0' after the body */
static ssize_t read_more(struct nw_req *req, int fd)
{
    if (req->len >= NW_REQ_MAX_SIZE)
        return fail(req, NW_REQ_ERR_TOO_LARGE, "response larger than %d bytes", NW_REQ_MAX_SIZE);
    if (req->len + 1 >= req->cap) {
        size_t cap = req->cap ? req->cap * 2 : 4096;
        char *data = realloc(req->data, cap);
        if (data == NULL)
            return fail(req, NW_REQ_ERR_MEMORY, "out of memory");
        req->data = data;
        req->cap = cap;
    }
    size_t room = req->cap - req->len - 1;
    if (room > NW_REQ_MAX_SIZE - req->len)
        room = NW_REQ_MAX_SIZE - req->len;

    for (;;) {
        ssize_t n = recv(fd, req->data + req->len, room, 0);
        if (n >= 0) {
            req->len += n;
            return n;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            int ret = wait_fd(req, fd, POLLIN, req->deadline);
            if (ret == 0)
                return fail(req, NW_REQ_ERR_TIMEOUT, "timeout waiting for the response");
            if (ret < 0)
                return fail(req, NW_REQ_ERR_IO, "poll: %m");
        } else if (errno != EINTR) {
            return fail(req, NW_REQ_ERR_IO, "recv: %m");
        }
    }
}

/* where "\r\n" is in data[from, to), or -1 */
static ssize_t find_crlf(const char *data, size_t from, size_t to)
{
    for (size_t i = from; i + 1 < to; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n')
            return i;
    }
    return -1;
}

/* cut the header lines into names and values, in place. data may still
 * move while the body is read, so offsets are kept, not pointers */
static void split_head(struct nw_req *req)
{
    char *data = req->data;
    size_t pos = find_crlf(data, 0, req->head_size) + 2;
    for (;;) {
        size_t end = find_crlf(data, pos, req->head_size);
        if (end == pos)
            break;
        size_t next = end + 2;
        char *colon = memchr(data + pos, ':', end - pos);
        if (colon && colon != data + pos && req->header_count < NW_REQ_MAX_HEADERS) {
            size_t name_end = colon - data;
            size_t value = name_end + 1;
            while (name_end > pos && (data[name_end - 1] == ' ' || data[name_end - 1] == '\t'))
                name_end--;
            while (value < end && (data[value] == ' ' || data[value] == '\t'))
                value++;
            while (end > value && (data[end - 1] == ' ' || data[end - 1] == '\t'))
                end--;
            data[name_end] = '\0';
            data[end] = '\0';
            req->offsets[req->header_count].name = pos;
            req->offsets[req->header_count].value = value;
            req->header_count++;
        }
        pos = next;
    }
}

static const char *head_value(struct nw_req *req, const char *name)
{
    for (uint32_t i = 0; i < req->header_count; ++i) {
        if (strcasecmp(req->data + req->offsets[i].name, name) == 0)
            return req->data + req->offsets[i].value;
    }
    return NULL;
}

static int read_head(struct nw_req *req, int fd)
{
    size_t scanned = 0;
    for (;;) {
        bool found = false;
        for (size_t i = scanned > 3 ? scanned - 3 : 0; i + 3 < req->len; ++i) {
            if (memcmp(req->data + i, "\r\n\r\n", 4) == 0) {
                req->head_size = i + 4;
                found = true;
                break;
            }
        }
        if (!found) {
            scanned = req->len;
            if (req->len > HEAD_MAX)
                return fail(req, NW_REQ_ERR_RESPONSE, "the head of the response is larger than %d bytes", HEAD_MAX);
            ssize_t n = read_more(req, fd);
            if (n < 0)
                return -1;
            if (n == 0) {
                if (req->len == 0)
                    return fail(req, NW_REQ_ERR_IO, "connection closed without a response");
                return fail(req, NW_REQ_ERR_RESPONSE, "connection closed in the head of the response");
            }
            continue;
        }

        /* "HTTP/1.1 200 OK" */
        const char *d = req->data;
        if (req->head_size < 16 || memcmp(d, "HTTP/1.", 7) != 0 || !isdigit((unsigned char)d[7]) || d[8] != ' ' ||
                !isdigit((unsigned char)d[9]) || !isdigit((unsigned char)d[10]) || !isdigit((unsigned char)d[11]) ||
                (d[12] != ' ' && d[12] != '\r')) {
            return fail(req, NW_REQ_ERR_RESPONSE, "not a http response");
        }
        req->status = (d[9] - '0') * 100 + (d[10] - '0') * 10 + (d[11] - '0');
        if (req->status >= 200)
            break;
        /* 100 Continue and the like: the response comes after it */
        memmove(req->data, req->data + req->head_size, req->len - req->head_size);
        req->len -= req->head_size;
        req->head_size = 0;
        scanned = 0;
    }
    split_head(req);
    return 0;
}

/*
 * a chunked body: "size in hex\r\n data \r\n" again and again, ended by a
 * chunk of size 0. the data is moved down over the size lines as it is
 * read, so that the body ends up in one piece where it started.
 */
static int read_chunked(struct nw_req *req, int fd)
{
    size_t from = req->head_size;       /* where the next size line is */
    size_t to = from;                   /* where the next data goes */
    for (;;) {
        ssize_t line_end;
        while ((line_end = find_crlf(req->data, from, req->len)) < 0) {
            if (req->len - from > CHUNK_LINE_MAX)
                return fail(req, NW_REQ_ERR_RESPONSE, "bad chunk in the response");
            ssize_t n = read_more(req, fd);
            if (n < 0)
                return -1;
            if (n == 0)
                return fail(req, NW_REQ_ERR_IO, "connection closed before the end of the response");
        }
        size_t size = 0;
        size_t digits = 0;
        for (size_t i = from; i < (size_t)line_end && isxdigit((unsigned char)req->data[i]); ++i, ++digits) {
            char c = req->data[i];
            size = size * 16 + (isdigit((unsigned char)c) ? c - '0' : tolower((unsigned char)c) - 'a' + 10);
            if (size > NW_REQ_MAX_SIZE)
                return fail(req, NW_REQ_ERR_TOO_LARGE, "response larger than %d bytes", NW_REQ_MAX_SIZE);
        }
        char after = req->data[from + digits];
        if (digits == 0 || (after != '\r' && after != ';' && after != ' ' && after != '\t'))
            return fail(req, NW_REQ_ERR_RESPONSE, "bad chunk in the response");
        if (size == 0)
            break;

        size_t data_at = line_end + 2;
        while (req->len < data_at + size + 2) {
            ssize_t n = read_more(req, fd);
            if (n < 0)
                return -1;
            if (n == 0)
                return fail(req, NW_REQ_ERR_IO, "connection closed before the end of the response");
        }
        if (req->data[data_at + size] != '\r' || req->data[data_at + size + 1] != '\n')
            return fail(req, NW_REQ_ERR_RESPONSE, "bad chunk in the response");
        memmove(req->data + to, req->data + data_at, size);
        to += size;
        from = data_at + size + 2;
    }
    req->body_size = to - req->head_size;
    return 0;
}

static int read_body(struct nw_req *req, int fd)
{
    if (req->status == 204 || req->status == 304)
        return 0;

    const char *encoding = head_value(req, "Transfer-Encoding");
    if (encoding) {
        size_t len = strlen(encoding);
        if (len >= 7 && strcasecmp(encoding + len - 7, "chunked") == 0)
            return read_chunked(req, fd);
    }

    const char *length = head_value(req, "Content-Length");
    if (length) {
        char *end;
        errno = 0;
        unsigned long long size = strtoull(length, &end, 10);
        if (!isdigit((unsigned char)length[0]) || *end != '\0' || errno != 0)
            return fail(req, NW_REQ_ERR_RESPONSE, "bad Content-Length in the response");
        if (size > NW_REQ_MAX_SIZE - req->head_size)
            return fail(req, NW_REQ_ERR_TOO_LARGE, "response larger than %d bytes", NW_REQ_MAX_SIZE);
        while (req->len - req->head_size < size) {
            ssize_t n = read_more(req, fd);
            if (n < 0)
                return -1;
            if (n == 0)
                return fail(req, NW_REQ_ERR_IO, "connection closed before the end of the response");
        }
        req->body_size = size;
        return 0;
    }

    /* neither: the body ends with the connection */
    for (;;) {
        ssize_t n = read_more(req, fd);
        if (n < 0)
            return -1;
        if (n == 0)
            break;
    }
    req->body_size = req->len - req->head_size;
    return 0;
}

/* on a request thread */
static void on_job(nw_job_entry *entry, void *privdata)
{
    struct nw_req *req = entry->request;
    if (given_up(req))
        return;
    if (monotonic() >= req->deadline) {
        fail(req, NW_REQ_ERR_TIMEOUT, "timeout before the request was sent");
        return;
    }
    int fd = open_connection(req);
    if (fd < 0)
        return;
    if (send_request(req, fd) == 0 && read_head(req, fd) == 0)
        read_body(req, fd);
    close(fd);
}

/* back on the loop thread */
static void on_finish(nw_job_entry *entry)
{
    struct nw_req *req = entry->request;
    /* nobody wants to know: cancelled, or made without a callback */
    if (req->cancelled || req->callback == NULL)
        return;
    nw_req_response *rsp = &req->rsp;
    if (req->error) {
        rsp->body = "";
        rsp->error_msg = req->error_msg;
    } else {
        rsp->status = req->status;
        rsp->header_count = req->header_count;
        for (uint32_t i = 0; i < req->header_count; ++i) {
            rsp->headers[i].name = req->data + req->offsets[i].name;
            rsp->headers[i].value = req->data + req->offsets[i].value;
        }
        rsp->body = req->data + req->head_size;
        rsp->body_size = req->body_size;
        rsp->body[rsp->body_size] = '\0';
        rsp->error_msg = "";
    }
    req->callback(req->error, rsp, req->privdata);
}

static void req_free(struct nw_req *req)
{
    free(req->host);
    free(req->text);
    free(req->data);
    free(req);
}

/* answered, cancelled or never made: the request ends here */
static void on_cleanup(nw_job_entry *entry)
{
    req_free(entry->request);
}

static void pool_release(void)
{
    if (pool) {
        /* the requests on their way stop at their next look */
        __atomic_store_n(&stopping, 1, __ATOMIC_RELAXED);
        nw_job_release(pool);
        __atomic_store_n(&stopping, 0, __ATOMIC_RELAXED);
        pool = NULL;
    }
}

static int pool_init(void)
{
    if (pool)
        return 0;
    nw_job_type type;
    memset(&type, 0, sizeof(type));
    type.on_job = on_job;
    type.on_finish = on_finish;
    type.on_cleanup = on_cleanup;
    pool = nw_job_create(&type, NW_REQ_THREADS);
    if (pool == NULL)
        return -1;
    /* the threads go when the loop of this thread goes */
    if (nw_loop_at_release(pool_release) < 0) {
        pool_release();
        return -1;
    }
    return 0;
}

/* "http://host[:port][/path][?query]" into what the worker needs: the
 * host and port to connect to, and the request as it is sent, with its
 * body if there is one (content_type is NULL for a request without) */
static int build_request(struct nw_req *req, const char *method, const char *url,
        const char *content_type, const void *body, size_t size)
{
    size_t url_len = strlen(url);
    if (url_len >= NW_REQ_MAX_URL || strncasecmp(url, "http://", 7) != 0)
        return -1;

    const char *p = url + 7;
    const char *host = p;
    bool ipv6 = false;
    if (*p == '[') {
        ipv6 = true;
        host = ++p;
        while (isxdigit((unsigned char)*p) || *p == ':' || *p == '.')
            p++;
        if (*p != ']')
            return -1;
    } else {
        while (isalnum((unsigned char)*p) || *p == '-' || *p == '.' || *p == '_')
            p++;
    }
    size_t host_len = p - host;
    if (host_len == 0 || host_len >= NI_MAXHOST)
        return -1;
    if (ipv6)
        p++;

    unsigned port = 80;
    if (*p == ':') {
        port = 0;
        size_t digits = 0;
        for (p++; isdigit((unsigned char)*p) && digits < 6; ++p, ++digits)
            port = port * 10 + (*p - '0');
        if (digits == 0 || port == 0 || port > 65535)
            return -1;
    }
    /* anything else here is not a url of ours: "user@host", a space ... */
    if (*p != '\0' && *p != '/' && *p != '?' && *p != '#')
        return -1;

    /* what follows the host goes into the request as it is, up to a '#'.
     * no space and no line break may get in there */
    size_t target_len = strcspn(p, "#");
    for (size_t i = 0; i < target_len; ++i) {
        if ((unsigned char)p[i] <= ' ' || p[i] == 0x7f)
            return -1;
    }

    char about_body[160] = "";
    if (content_type) {
        /* a line of the head: it must not bring lines of its own */
        size_t type_len = strlen(content_type);
        if (type_len == 0 || type_len > 100)
            return -1;
        for (size_t i = 0; i < type_len; ++i) {
            if ((unsigned char)content_type[i] < ' ' || content_type[i] == 0x7f)
                return -1;
        }
        snprintf(about_body, sizeof(about_body), "Content-Type: %s\r\nContent-Length: %zu\r\n", content_type, size);
    }

    req->host = strndup(host, host_len);
    req->text = malloc(url_len + host_len + 320 + size);
    if (req->host == NULL || req->text == NULL)
        return -1;
    snprintf(req->port, sizeof(req->port), "%u", port);
    char port_text[8] = "";
    if (port != 80)
        snprintf(port_text, sizeof(port_text), ":%u", port);
    req->text_size = sprintf(req->text,
            "%s %s%.*s HTTP/1.1\r\n"
            "Host: %s%s%s%s\r\n"
            "User-Agent: asnw\r\n"
            "Accept: */*\r\n"
            "%s"
            "Connection: close\r\n"
            "\r\n",
            method, p[0] == '/' ? "" : "/", (int)target_len, p,
            ipv6 ? "[" : "", req->host, ipv6 ? "]" : "", port_text, about_body);
    if (size > 0) {
        memcpy(req->text + req->text_size, body, size);
        req->text_size += size;
    }
    return 0;
}

static nw_req *request(const char *method, const char *url, const char *content_type, const void *body, size_t size,
        double timeout, nw_req_callback callback, void *privdata)
{
    struct nw_req *req = malloc(sizeof(struct nw_req));
    if (req == NULL)
        return NULL;
    memset(req, 0, sizeof(struct nw_req));
    if (build_request(req, method, url, content_type, body, size) < 0 || pool_init() < 0) {
        req_free(req);
        return NULL;
    }
    req->deadline = monotonic() + (timeout > 0 ? timeout : NW_REQ_DEFAULT_TIMEOUT);
    req->callback = callback;
    req->privdata = privdata;
    req->stopping = &stopping;
    if (nw_job_add(pool, 0, req) < 0) {
        req_free(req);
        return NULL;
    }
    return req;
}

nw_req *nw_req_get(const char *url, double timeout, nw_req_callback callback, void *privdata)
{
    return request("GET", url, NULL, NULL, 0, timeout, callback, privdata);
}

nw_req *nw_req_post(const char *url, const char *content_type, const void *body, size_t size,
        double timeout, nw_req_callback callback, void *privdata)
{
    if (size > NW_REQ_MAX_SIZE || (body == NULL && size > 0))
        return NULL;
    if (content_type == NULL)
        content_type = "application/json";
    return request("POST", url, content_type, body, size, timeout, callback, privdata);
}

void nw_req_cancel(nw_req *req)
{
    __atomic_store_n(&req->cancelled, 1, __ATOMIC_RELAXED);
}

int nw_req_url_encode(const void *data, size_t size, char *dest, size_t dest_size)
{
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char *src = data;
    size_t len = 0;
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = src[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            if (len + 1 >= dest_size)
                return -1;
            dest[len++] = c;
        } else {
            if (len + 3 >= dest_size)
                return -1;
            dest[len++] = '%';
            dest[len++] = hex[c >> 4];
            dest[len++] = hex[c & 15];
        }
    }
    if (len >= dest_size)
        return -1;
    dest[len] = '\0';
    return len;
}

char *nw_req_get_header(nw_req_response *rsp, const char *name)
{
    for (uint32_t i = 0; i < rsp->header_count; ++i) {
        if (strcasecmp(rsp->headers[i].name, name) == 0)
            return rsp->headers[i].value;
    }
    return NULL;
}
