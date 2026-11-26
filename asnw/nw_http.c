/*
 * Description: http server, HTTP/1.0 and HTTP/1.1 on top of nw_svr
 *     History: 2026/11/25, create
 */

# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <strings.h>
# include <time.h>

# include "nw_http.h"

# define NW_HTTP_DEFAULT_MAX_REQUEST    (64 * 1024)
# define NW_HTTP_MAX_REQUEST_LIMIT      (1024 * 1024 * 1024)
# define NW_HTTP_DEFAULT_KEEP_ALIVE     60.0
# define NW_HTTP_DEFAULT_REQUEST_TIME   10.0
/* a response whose head and body fit in this many bytes goes out in one
 * write, bigger bodies are written separately and never copied */
# define NW_HTTP_SEND_BUF_SIZE          (16 * 1024)
# define NW_HTTP_CHUNK_LINE_MAX         64

/*
 * reading, step 1: framing
 *
 * find out where a request ends without looking at anything else. this runs
 * every time more data arrives (it is the decode_pkg of the connection), so
 * it keeps its progress in nw_http_frame and never looks at a byte twice.
 * it does not modify the data.
 */

typedef struct nw_http_frame {
    /* bytes already searched for the end of the head */
    uint32_t scanned;
    /* size of the head including the empty line, 0 until it is complete */
    uint32_t head_size;
    /* chunked: offset of the next chunk size line */
    uint32_t chunk_pos;
    /* size of the body: Content-Length, or the chunks seen so far */
    uint64_t body_size;
    bool chunked;
    bool expect_continue;
} nw_http_frame;

/* token characters of RFC 9110: what methods and header names are made of */
static bool is_tchar(unsigned char c)
{
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
        return true;
    return c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static bool is_ows(char c)
{
    return c == ' ' || c == '\t';
}

/* strip optional white space on both sides of [*start, *end) */
static void trim_ows(const char **start, const char **end)
{
    while (*start < *end && is_ows(**start))
        (*start)++;
    while (*end > *start && is_ows((*end)[-1]))
        (*end)--;
}

static bool equal_nocase(const char *s, size_t len, const char *name)
{
    return strlen(name) == len && strncasecmp(s, name, len) == 0;
}

/* the headers that decide where the request ends. return 0 or -status */
static int frame_headers(nw_http_frame *f, const char *data, size_t limit)
{
    bool has_length = false;
    const char *end = data + f->head_size - 2;
    const char *pos = memchr(data, '\n', f->head_size) + 1;

    while (pos < end) {
        const char *line_end = (const char *)memchr(pos, '\n', end - pos) - 1;
        const char *colon = memchr(pos, ':', line_end - pos);
        if (colon == NULL)
            return -400;
        size_t name_len = colon - pos;
        const char *value = colon + 1;
        const char *value_end = line_end;
        trim_ows(&value, &value_end);
        size_t value_len = value_end - value;

        if (equal_nocase(pos, name_len, "Content-Length")) {
            /* one, made of digits only. two of them, or one a proxy in front
             * might read differently, is how requests get smuggled */
            if (has_length || value_len == 0 || value_len > 18)
                return -400;
            uint64_t length = 0;
            for (size_t i = 0; i < value_len; ++i) {
                if (value[i] < '0' || value[i] > '9')
                    return -400;
                length = length * 10 + (value[i] - '0');
            }
            has_length = true;
            f->body_size = length;
        } else if (equal_nocase(pos, name_len, "Transfer-Encoding")) {
            if (f->chunked)
                return -400;
            if (!equal_nocase(value, value_len, "chunked"))
                return -501;
            f->chunked = true;
        } else if (equal_nocase(pos, name_len, "Expect")) {
            if (equal_nocase(value, value_len, "100-continue"))
                f->expect_continue = true;
        }
        pos = line_end + 2;
    }

    if (f->chunked && has_length)
        return -400;
    if (f->chunked)
        f->body_size = 0;
    else if (f->head_size + f->body_size > limit)
        return -413;

    return 0;
}

/*
 * return the size of the request at data if it is complete within max
 * bytes, 0 if more data is needed, or -status if it has to be refused.
 * limit is the biggest request that is acceptable.
 */
static int frame_request(nw_http_frame *f, const char *data, size_t max, size_t limit)
{
    if (f->head_size == 0) {
        /* not text at all (a TLS handshake ...): no point in waiting */
        if (f->scanned == 0 && max > 0 && !is_tchar(data[0]))
            return -400;
        size_t pos = f->scanned;
        while (pos < max) {
            const char *nl = memchr(data + pos, '\n', max - pos);
            if (nl == NULL) {
                pos = max;
                break;
            }
            pos = nl - data;
            /* every line ends with CR LF, a bare LF is refused */
            if (pos == 0 || data[pos - 1] != '\r')
                return -400;
            if (pos >= 3 && data[pos - 3] == '\r' && data[pos - 2] == '\n') {
                f->head_size = pos + 1;
                break;
            }
            pos += 1;
        }
        if (f->head_size == 0) {
            f->scanned = pos;
            return 0;
        }
        int ret = frame_headers(f, data, limit);
        if (ret < 0)
            return ret;
        f->chunk_pos = f->head_size;
    }

    if (!f->chunked) {
        size_t total = f->head_size + f->body_size;
        return max >= total ? (int)total : 0;
    }

    /* chunked: <hex size>[;extension] CR LF <data> CR LF ..., ended by a
     * chunk of size 0, optional trailer lines and an empty line */
    for (;;) {
        size_t pos = f->chunk_pos;
        const char *nl = memchr(data + pos, '\n', max - pos);
        if (nl == NULL)
            return max - pos > NW_HTTP_CHUNK_LINE_MAX ? -400 : 0;
        size_t line_end = nl - data;
        if (line_end == pos || data[line_end - 1] != '\r' || line_end - pos > NW_HTTP_CHUNK_LINE_MAX)
            return -400;

        uint64_t size = 0;
        size_t digits = 0;
        for (; pos + digits < line_end - 1; ++digits) {
            char c = data[pos + digits];
            int value;
            if (c >= '0' && c <= '9')
                value = c - '0';
            else if (c >= 'a' && c <= 'f')
                value = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                value = c - 'A' + 10;
            else if (c == ';' && digits > 0)
                break;
            else
                return -400;
            if (digits >= 8)
                return -400;
            size = size * 16 + value;
        }
        if (digits == 0)
            return -400;

        if (size == 0) {
            /* skip the trailer lines up to the empty one */
            size_t cur = line_end + 1;
            for (;;) {
                nl = memchr(data + cur, '\n', max - cur);
                if (nl == NULL)
                    return 0;
                size_t trailer_end = nl - data;
                if (trailer_end == cur || data[trailer_end - 1] != '\r')
                    return -400;
                if (trailer_end == cur + 1)
                    return (int)(trailer_end + 1);
                cur = trailer_end + 1;
            }
        }

        size_t chunk_end = line_end + 1 + size + 2;
        if (chunk_end > limit)
            return -413;
        if (max < chunk_end)
            return 0;
        if (data[chunk_end - 2] != '\r' || data[chunk_end - 1] != '\n')
            return -400;
        f->body_size += size;
        f->chunk_pos = chunk_end;
    }
}

/*
 * reading, step 2: parsing
 *
 * runs once, when the whole request is in one piece of memory. cuts it into
 * the strings of a nw_http_request, in place.
 */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int nw_http_url_decode(char *data, size_t size, bool plus_as_space)
{
    size_t out = 0;
    for (size_t in = 0; in < size; ++in) {
        char c = data[in];
        if (c == '%') {
            if (in + 2 >= size)
                return -1;
            int high = hex_value(data[in + 1]);
            int low = hex_value(data[in + 2]);
            if (high < 0 || low < 0)
                return -1;
            c = (char)(high * 16 + low);
            in += 2;
        } else if (c == '+' && plus_as_space) {
            c = ' ';
        }
        data[out++] = c;
    }
    if (out < size)
        data[out] = '\0';
    return (int)out;
}

/* does a comma separated header value hold this token */
static bool has_token(const char *value, const char *token)
{
    const char *pos = value;
    const char *end = value + strlen(value);
    while (pos < end) {
        const char *comma = memchr(pos, ',', end - pos);
        const char *item_end = comma ? comma : end;
        const char *item = pos;
        trim_ows(&item, &item_end);
        if (equal_nocase(item, item_end - item, token))
            return true;
        pos = comma ? comma + 1 : end;
    }
    return false;
}

/* METHOD SP target SP HTTP/1.x, line is [line, line_end) */
static int parse_request_line(char *line, char *line_end, nw_http_request *req)
{
    char *sp1 = memchr(line, ' ', line_end - line);
    if (sp1 == NULL || sp1 == line)
        return 400;
    for (char *p = line; p < sp1; ++p) {
        if (!is_tchar(*p))
            return 400;
    }
    char *target = sp1 + 1;
    char *sp2 = memchr(target, ' ', line_end - target);
    if (sp2 == NULL || sp2 == target)
        return 400;
    /* the target: origin form (/path?query), or "*" for OPTIONS */
    if (target[0] != '/' && !(target[0] == '*' && sp2 == target + 1))
        return 400;
    for (char *p = target; p < sp2; ++p) {
        if ((unsigned char)*p <= 0x20 || *p == 0x7f)
            return 400;
    }

    char *version = sp2 + 1;
    if (line_end - version != 8 || memcmp(version, "HTTP/", 5) != 0)
        return 400;
    if (memcmp(version, "HTTP/1.1", 8) == 0)
        req->version_minor = 1;
    else if (memcmp(version, "HTTP/1.0", 8) == 0)
        req->version_minor = 0;
    else
        return 505;

    *sp1 = '\0';
    req->method = line;

    *sp2 = '\0';
    char *question = memchr(target, '?', sp2 - target);
    char *path_end = question ? question : sp2;
    if (question) {
        *question = '\0';
        req->query = question + 1;
    } else {
        req->query = sp2;
    }
    int path_len = nw_http_url_decode(target, path_end - target, false);
    if (path_len < 0 || memchr(target, '\0', path_len) != NULL)
        return 400;
    req->path = target;

    return 0;
}

/* name ":" OWS value OWS, line is [line, line_end) and line_end[0] is CR */
static int parse_header_line(char *line, char *line_end, nw_http_header *header)
{
    char *colon = memchr(line, ':', line_end - line);
    /* no name, or white space in front of the colon, or a folded line (it
     * starts with white space): all of them fail the token check */
    if (colon == NULL || colon == line)
        return 400;
    for (char *p = line; p < colon; ++p) {
        if (!is_tchar(*p))
            return 400;
    }
    const char *value = colon + 1;
    const char *value_end = line_end;
    trim_ows(&value, &value_end);
    for (const char *p = value; p < value_end; ++p) {
        unsigned char c = *p;
        if ((c < 0x20 && c != '\t') || c == 0x7f)
            return 400;
    }

    *colon = '\0';
    header->name = line;
    /* value_end is at trailing white space or at the CR: free to overwrite */
    *(char *)value_end = '\0';
    header->value = (char *)value;

    return 0;
}

/* move the data of every chunk together, right behind the head */
static void dechunk(nw_http_frame *f, char *data, nw_http_request *req)
{
    char *out = data + f->head_size;
    char *pos = out;
    req->body = out;
    for (;;) {
        char *line_end = memchr(pos, '\n', NW_HTTP_CHUNK_LINE_MAX + 2);
        size_t size = strtoul(pos, NULL, 16);
        if (size == 0)
            break;
        memmove(out, line_end + 1, size);
        out += size;
        pos = line_end + 1 + size + 2;
    }
    req->body_size = out - (char *)req->body;
}

/* the request at data is complete and framed by f. return 0 or a status */
static int parse_request(nw_http_frame *f, char *data, nw_http_request *req)
{
    char *head_end = data + f->head_size - 2;
    char *line_end = (char *)memchr(data, '\n', f->head_size) - 1;
    int ret = parse_request_line(data, line_end, req);
    if (ret != 0)
        return ret;

    const char *connection = NULL;
    uint32_t host_count = 0;
    req->header_count = 0;
    char *pos = line_end + 2;
    while (pos < head_end) {
        line_end = (char *)memchr(pos, '\n', head_end - pos) - 1;
        if (req->header_count == NW_HTTP_MAX_HEADERS)
            return 431;
        nw_http_header *header = &req->headers[req->header_count];
        ret = parse_header_line(pos, line_end, header);
        if (ret != 0)
            return ret;
        req->header_count++;
        if (strcasecmp(header->name, "Connection") == 0)
            connection = header->value;
        else if (strcasecmp(header->name, "Host") == 0)
            host_count++;
        pos = line_end + 2;
    }

    /* HTTP/1.1 requires exactly one Host */
    if (host_count > 1 || (host_count == 0 && req->version_minor == 1))
        return 400;
    if (req->version_minor == 1)
        req->keep_alive = !(connection && has_token(connection, "close"));
    else
        req->keep_alive = connection && has_token(connection, "keep-alive");

    if (f->chunked) {
        if (req->version_minor == 0)
            return 400;
        dechunk(f, data, req);
    } else {
        req->body = data + f->head_size;
        req->body_size = f->body_size;
    }

    return 0;
}

int nw_http_parse_request(char *data, size_t size, nw_http_request *req)
{
    nw_http_frame frame;
    memset(&frame, 0, sizeof(frame));
    /* no size limit here: a request that claims to be longer than what was
     * given is simply incomplete */
    int ret = frame_request(&frame, data, size, SIZE_MAX);
    if (ret < 0)
        return -ret;
    if (ret == 0 || (size_t)ret != size)
        return 400;
    return parse_request(&frame, data, req);
}

bool nw_http_header_is(nw_http_header *header, const char *name)
{
    return strcasecmp(header->name, name) == 0;
}

char *nw_http_get_header(nw_http_request *req, const char *name)
{
    for (uint32_t i = 0; i < req->header_count; ++i) {
        nw_http_header *header = &req->headers[i];
        if (strcasecmp(header->name, name) == 0)
            return header->value;
    }
    return NULL;
}

/*
 * writing
 */

const char *nw_http_status_text(int status)
{
    switch (status) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 422: return "Unprocessable Content";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

int nw_http_add_header(nw_http_response *rsp, const char *name, const char *value)
{
    if (rsp->header_count >= NW_HTTP_MAX_HEADERS)
        return -1;
    nw_http_header *header = &rsp->headers[rsp->header_count++];
    header->name = (char *)name;
    header->value = (char *)value;
    return 0;
}

/* a line break in a header would let its content pose as further headers
 * or as a second response */
static bool has_line_break(const char *s, size_t len)
{
    return memchr(s, '\r', len) != NULL || memchr(s, '\n', len) != NULL;
}

/*
 * server
 */

/* what the server keeps per connection, it is the privdata of the session */
typedef struct nw_http_conn {
    nw_http_frame frame;
    /* status to refuse the request with, found while framing */
    int error;
    /* a request was handed to on_request and is not answered yet */
    bool pending;
    /* that request: was it a HEAD, does the client want keep-alive */
    bool head_request;
    bool keep_alive;
    /* a request arrived while one was pending: close after the answer */
    bool close_after_reply;
    bool continue_sent;
    /* a request is arriving and has until its deadline to be complete */
    bool receiving;
} nw_http_conn;

nw_http_svr *nw_http_svr_of(nw_ses *ses)
{
    return ((nw_svr *)ses->svr)->privdata;
}

static const char *http_date(nw_http_svr *svr)
{
    time_t t = (time_t)ev_now(nw_default_loop);
    if (t != svr->date_time) {
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(svr->date, sizeof(svr->date), "%a, %d %b %Y %H:%M:%S GMT", &tm);
        svr->date_time = t;
    }
    return svr->date;
}

static bool is_auto_header(nw_http_header *header)
{
    return nw_http_header_is(header, "Content-Length") || nw_http_header_is(header, "Connection") || \
        nw_http_header_is(header, "Transfer-Encoding");
}

/* send a response on a connection and decide what becomes of it. the
 * session may be closed when this returns */
static int http_send(nw_http_svr *svr, nw_ses *ses, nw_http_conn *conn, nw_http_response *rsp, bool keep_alive)
{
    const char *reason = rsp->reason ? rsp->reason : nw_http_status_text(rsp->status);
    /* these never have a body, and say nothing about its length */
    bool bodyless = rsp->status < 200 || rsp->status == 204 || rsp->status == 304;
    size_t body_size = bodyless ? 0 : rsp->body_size;
    bool has_date = false;

    if (rsp->status < 100 || rsp->status > 999 || has_line_break(reason, strlen(reason)))
        return -1;
    size_t need = 160 + strlen(reason);
    for (uint32_t i = 0; i < rsp->header_count; ++i) {
        nw_http_header *header = &rsp->headers[i];
        size_t name_len = strlen(header->name);
        size_t value_len = strlen(header->value);
        if (name_len == 0 || has_line_break(header->name, name_len) || has_line_break(header->value, value_len))
            return -1;
        need += name_len + value_len + 4;
    }
    if (need > svr->send_buf_size) {
        size_t new_size = svr->send_buf_size;
        while (new_size < need)
            new_size *= 2;
        char *new_buf = realloc(svr->send_buf, new_size);
        if (new_buf == NULL)
            return -1;
        svr->send_buf = new_buf;
        svr->send_buf_size = new_size;
    }

    char *buf = svr->send_buf;
    size_t len = sprintf(buf, "HTTP/1.1 %d %s\r\n", rsp->status, reason);
    for (uint32_t i = 0; i < rsp->header_count; ++i) {
        nw_http_header *header = &rsp->headers[i];
        if (is_auto_header(header))
            continue;
        if (nw_http_header_is(header, "Date"))
            has_date = true;
        len += sprintf(buf + len, "%s: %s\r\n", header->name, header->value);
    }
    if (!has_date)
        len += sprintf(buf + len, "Date: %s\r\n", http_date(svr));
    if (!bodyless)
        len += sprintf(buf + len, "Content-Length: %zu\r\n", rsp->body_size);
    len += sprintf(buf + len, "Connection: %s\r\n\r\n", keep_alive ? "keep-alive" : "close");

    /* a HEAD gets the head it would get for a GET, and no body */
    if (conn->head_request)
        body_size = 0;

    uint64_t id = ses->id;
    conn->pending = false;

    int ret;
    if (body_size > 0 && len + body_size <= svr->send_buf_size) {
        memcpy(buf + len, rsp->body, body_size);
        ret = nw_ses_send(ses, buf, len + body_size);
    } else {
        ret = nw_ses_send(ses, buf, len);
        if (ret == 0 && body_size > 0)
            ret = nw_ses_send(ses, rsp->body, body_size);
    }
    /* a failed send closed the session and its memory may be reused: go by
     * the id to find out, not by the pointer */
    if (ret < 0 || nw_svr_get(svr->raw_svr, id) != ses)
        return -1;

    if (keep_alive) {
        /* idle again: the keep-alive timeout applies from here */
        nw_svr_set_idle_timeout(svr->raw_svr, ses, svr->keep_alive_timeout);
    } else {
        /* an orderly close, so that the response is not lost to a reset if
         * the client is still sending */
        nw_svr_shutdown(svr->raw_svr, ses);
    }

    return 0;
}

/* refuse a request: a short text response, then the connection is closed */
static void http_refuse(nw_http_svr *svr, nw_ses *ses, nw_http_conn *conn, int status)
{
    char body[64];
    nw_http_response rsp;
    rsp.status = status;
    rsp.reason = NULL;
    rsp.close = true;
    rsp.header_count = 0;
    nw_http_add_header(&rsp, "Content-Type", "text/plain");
    rsp.body = body;
    rsp.body_size = snprintf(body, sizeof(body), "%d %s\n", status, nw_http_status_text(status));

    if (svr->type.on_error_msg) {
        char msg[100];
        snprintf(msg, sizeof(msg), "request refused: %d %s", status, nw_http_status_text(status));
        svr->type.on_error_msg(ses, msg);
    }
    conn->head_request = false;
    http_send(svr, ses, conn, &rsp, false);
}

int nw_http_reply(nw_ses *ses, nw_http_response *rsp)
{
    if (ses->ses_type != NW_SES_TYPE_COMMON || ses->id == 0)
        return -1;
    nw_http_svr *svr = nw_http_svr_of(ses);
    nw_http_conn *conn = ses->privdata;
    if (!conn->pending || ses->closing)
        return -1;
    bool keep_alive = conn->keep_alive && !rsp->close && !conn->close_after_reply;
    return http_send(svr, ses, conn, rsp, keep_alive);
}

/*
 * with threads a response for a connection of another thread travels
 * there as one block: this header, the lengths of the header names and
 * values, then all the strings and the body.
 */
struct reply_request {
    uint64_t id;
    int status;
    bool close;
    uint32_t reason_len;
    uint32_t header_count;
    size_t body_size;
    /* the reason, the header names and values, each with its '\0', then
     * the body */
    char data[];
};

/* on the worker of the connection: put the response together again */
static void on_reply_request(nw_hub *hub, void *data, size_t size)
{
    struct reply_request *request = data;
    nw_ses *ses = nw_svr_get(nw_hub_svr(hub), request->id);
    if (ses == NULL)
        return;

    nw_http_response rsp;
    char *p = request->data;
    rsp.status = request->status;
    rsp.close = request->close;
    rsp.reason = NULL;
    if (request->reason_len > 0) {
        /* sent along with its '\0' */
        rsp.reason = p;
        p += request->reason_len;
    }
    rsp.header_count = request->header_count;
    for (uint32_t i = 0; i < rsp.header_count; ++i) {
        rsp.headers[i].name = p;
        p += strlen(p) + 1;
        rsp.headers[i].value = p;
        p += strlen(p) + 1;
    }
    rsp.body = p;
    rsp.body_size = request->body_size;
    nw_http_reply(ses, &rsp);
}

static int post_reply(nw_http_svr *svr, uint64_t id, nw_http_response *rsp)
{
    if (id == 0 || rsp->header_count > NW_HTTP_MAX_HEADERS)
        return -1;
    size_t reason_len = rsp->reason ? strlen(rsp->reason) + 1 : 0;
    size_t size = sizeof(struct reply_request) + reason_len + rsp->body_size;
    for (uint32_t i = 0; i < rsp->header_count; ++i)
        size += strlen(rsp->headers[i].name) + strlen(rsp->headers[i].value) + 2;

    struct reply_request *request = malloc(size);
    if (request == NULL)
        return -1;
    memset(request, 0, sizeof(struct reply_request));
    request->id = id;
    request->status = rsp->status;
    request->close = rsp->close;
    request->reason_len = reason_len;
    request->header_count = rsp->header_count;
    request->body_size = rsp->body_size;
    char *p = request->data;
    if (reason_len > 0) {
        memcpy(p, rsp->reason, reason_len);
        p += reason_len;
    }
    for (uint32_t i = 0; i < rsp->header_count; ++i) {
        nw_http_header *header = &rsp->headers[i];
        size_t name_size = strlen(header->name) + 1;
        size_t value_size = strlen(header->value) + 1;
        memcpy(p, header->name, name_size);
        p += name_size;
        memcpy(p, header->value, value_size);
        p += value_size;
    }
    if (rsp->body_size > 0)
        memcpy(p, rsp->body, rsp->body_size);

    int ret = nw_hub_run(svr->hub, nw_hub_index_of(id), on_reply_request, request, size);
    free(request);
    return ret;
}

int nw_http_reply_id(nw_http_svr *svr, uint64_t id, nw_http_response *rsp)
{
    if (svr->hub) {
        /* svr may be the server of any thread, or the front one: the id
         * tells which worker the connection is on */
        if (nw_hub_index(svr->hub) != (int)nw_hub_index_of(id))
            return post_reply(svr, id, rsp);
        svr = nw_hub_svr(svr->hub)->privdata;
    }
    nw_ses *ses = nw_svr_get(svr->raw_svr, id);
    if (ses == NULL)
        return -1;
    return nw_http_reply(ses, rsp);
}

int nw_http_reply_body(nw_ses *ses, int status, const char *content_type, const void *body, size_t size)
{
    nw_http_response rsp;
    rsp.status = status;
    rsp.reason = NULL;
    rsp.close = false;
    rsp.header_count = 0;
    if (content_type)
        nw_http_add_header(&rsp, "Content-Type", content_type);
    rsp.body = body;
    rsp.body_size = size;
    return nw_http_reply(ses, &rsp);
}

/* "100 Continue" can't be sent from decode_pkg, which must not send. the
 * connection is noted here and served by a zero delay timer */
static void on_continue_timer(nw_timer *timer, void *privdata)
{
    static const char msg[] = "HTTP/1.1 100 Continue\r\n\r\n";
    nw_http_svr *svr = privdata;
    for (uint32_t i = 0; i < svr->continue_count; ++i) {
        nw_ses *ses = nw_svr_get(svr->raw_svr, svr->continue_ids[i]);
        if (ses == NULL)
            continue;
        nw_http_conn *conn = ses->privdata;
        if (conn->receiving)
            nw_ses_send(ses, msg, sizeof(msg) - 1);
    }
    svr->continue_count = 0;
}

static void want_continue(nw_http_svr *svr, nw_ses *ses)
{
    if (svr->continue_count == svr->continue_total) {
        uint32_t new_total = svr->continue_total ? svr->continue_total * 2 : 16;
        uint64_t *new_ids = realloc(svr->continue_ids, new_total * sizeof(uint64_t));
        if (new_ids == NULL)
            return;
        svr->continue_ids = new_ids;
        svr->continue_total = new_total;
    }
    svr->continue_ids[svr->continue_count++] = ses->id;
    if (!nw_timer_active(&svr->continue_timer)) {
        nw_timer_set(&svr->continue_timer, 0, false, on_continue_timer, svr);
        nw_timer_start(&svr->continue_timer);
    }
}

static int decode_pkg(nw_ses *ses, void *data, size_t max)
{
    nw_http_svr *svr = nw_http_svr_of(ses);
    nw_http_conn *conn = ses->privdata;
    char *s = data;

    if (conn->error)
        return max;

    if (conn->frame.scanned == 0 && conn->frame.head_size == 0) {
        /* empty lines between requests are tolerated, on_recv_pkg drops them */
        if (s[0] == '\r') {
            if (max < 2)
                return 0;
            if (s[1] == '\n')
                return 2;
        }
        if (!conn->receiving) {
            /* the first bytes of a request: all of it has to be here in
             * time, however slowly the rest trickles in */
            conn->receiving = true;
            nw_svr_set_deadline(svr->raw_svr, ses, svr->request_timeout);
        }
    }

    int ret = frame_request(&conn->frame, s, max, svr->max_request_size);
    if (ret == 0 && max >= svr->max_request_size) {
        /* the buffer is full and the request is not: it will never fit */
        ret = conn->frame.head_size ? -413 : -431;
    }
    if (ret < 0) {
        /* decode_pkg must not send: hand what is there to on_recv_pkg as a
         * message, it sends the refusal */
        conn->error = -ret;
        return max;
    }
    if (ret == 0 && conn->frame.head_size && conn->frame.expect_continue && !conn->continue_sent) {
        conn->continue_sent = true;
        want_continue(svr, ses);
    }

    return ret;
}

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    nw_http_svr *svr = nw_http_svr_of(ses);
    nw_http_conn *conn = ses->privdata;
    char *s = data;

    if (!conn->error && size == 2 && s[0] == '\r' && s[1] == '\n')
        return;

    nw_http_frame frame = conn->frame;
    int error = conn->error;
    memset(&conn->frame, 0, sizeof(conn->frame));
    conn->continue_sent = false;
    conn->receiving = false;
    nw_svr_set_deadline(svr->raw_svr, ses, 0);

    if (conn->pending) {
        /* pipelining is not supported: this request is dropped and the
         * connection ends with the answer to the one before */
        conn->error = 0;
        conn->close_after_reply = true;
        return;
    }
    if (error) {
        http_refuse(svr, ses, conn, error);
        return;
    }

    nw_http_request req;
    int status = parse_request(&frame, s, &req);
    if (status != 0) {
        http_refuse(svr, ses, conn, status);
        return;
    }

    /* the handler may take as long as it needs: no idle timeout while a
     * request is waiting for its answer */
    nw_svr_set_idle_timeout(svr->raw_svr, ses, 0);
    conn->pending = true;
    conn->keep_alive = req.keep_alive;
    conn->head_request = strcmp(req.method, "HEAD") == 0;
    svr->type.on_request(ses, &req);
}

static void on_open(nw_ses *ses)
{
    nw_http_svr *svr = nw_http_svr_of(ses);
    if (svr->type.on_open)
        svr->type.on_open(ses);
}

static void on_close(nw_ses *ses)
{
    nw_http_svr *svr = nw_http_svr_of(ses);
    if (svr->type.on_close)
        svr->type.on_close(ses);
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    nw_http_svr *svr = nw_http_svr_of(ses);
    if (svr->type.on_error_msg)
        svr->type.on_error_msg(ses, msg);
}

static void *on_privdata_alloc(void *raw_svr)
{
    nw_http_svr *svr = ((nw_svr *)raw_svr)->privdata;
    nw_http_conn *conn = nw_cache_alloc(svr->conn_cache);
    if (conn)
        memset(conn, 0, sizeof(nw_http_conn));
    return conn;
}

static void on_privdata_free(void *raw_svr, void *privdata)
{
    nw_http_svr *svr = ((nw_svr *)raw_svr)->privdata;
    nw_cache_free(svr->conn_cache, privdata);
}

/* the idle timeout or the request deadline of a connection has passed */
static void on_timeout(nw_ses *ses)
{
    nw_http_svr *svr = nw_http_svr_of(ses);
    nw_http_conn *conn = ses->privdata;
    if (conn->pending) {
        /* waiting for the handler, which is not the client's fault */
        return;
    }
    if (conn->receiving) {
        /* a request that was started and not finished in time */
        http_refuse(svr, ses, conn, 408);
    } else {
        /* a keep-alive connection nobody uses any more */
        nw_svr_close(svr->raw_svr, ses);
    }
}

static void http_svr_free(nw_http_svr *svr)
{
    nw_timer_stop(&svr->continue_timer);
    if (svr->raw_svr)
        nw_svr_release(svr->raw_svr);
    if (svr->conn_cache)
        nw_cache_release(svr->conn_cache);
    free(svr->continue_ids);
    free(svr->send_buf);
    free(svr);
}

/*
 * the settings, and what the nw_svr underneath is made from. with threads
 * this runs once for the front server (with_svr false: its nw_svr is the
 * accepting one of the hub) and once on every worker thread.
 */
static nw_http_svr *http_svr_new(nw_http_svr_cfg *cfg, nw_http_svr_type *type, void *privdata, bool with_svr,
        nw_svr_cfg *raw_cfg_out, nw_svr_type *raw_type_out)
{
    nw_http_svr *svr = malloc(sizeof(nw_http_svr));
    if (svr == NULL)
        return NULL;
    memset(svr, 0, sizeof(nw_http_svr));
    svr->type = *type;
    svr->privdata = privdata;
    svr->max_request_size = cfg->max_request_size ? cfg->max_request_size : NW_HTTP_DEFAULT_MAX_REQUEST;
    if (svr->max_request_size > NW_HTTP_MAX_REQUEST_LIMIT)
        svr->max_request_size = NW_HTTP_MAX_REQUEST_LIMIT;
    svr->keep_alive_timeout = cfg->keep_alive_timeout > 0 ? cfg->keep_alive_timeout : NW_HTTP_DEFAULT_KEEP_ALIVE;
    svr->request_timeout = cfg->request_timeout > 0 ? cfg->request_timeout : NW_HTTP_DEFAULT_REQUEST_TIME;
    svr->send_buf_size = NW_HTTP_SEND_BUF_SIZE;
    svr->send_buf = malloc(svr->send_buf_size);
    svr->conn_cache = nw_cache_create(sizeof(nw_http_conn));
    if (svr->send_buf == NULL || svr->conn_cache == NULL) {
        http_svr_free(svr);
        return NULL;
    }

    nw_svr_cfg raw_cfg;
    memset(&raw_cfg, 0, sizeof(raw_cfg));
    raw_cfg.bind_count = cfg->bind_count;
    raw_cfg.binds = cfg->binds;
    raw_cfg.max_pkg_size = svr->max_request_size;
    raw_cfg.buf_limit = cfg->buf_limit;
    raw_cfg.read_mem = cfg->read_mem;
    raw_cfg.write_mem = cfg->write_mem;
    raw_cfg.idle_timeout = svr->keep_alive_timeout;

    nw_svr_type raw_type;
    memset(&raw_type, 0, sizeof(raw_type));
    raw_type.decode_pkg = decode_pkg;
    raw_type.on_recv_pkg = on_recv_pkg;
    raw_type.on_open = on_open;
    raw_type.on_close = on_close;
    raw_type.on_error_msg = on_error_msg;
    raw_type.on_timeout = on_timeout;
    raw_type.on_drain = type->on_drain;
    raw_type.on_privdata_alloc = on_privdata_alloc;
    raw_type.on_privdata_free = on_privdata_free;

    if (with_svr) {
        svr->raw_svr = nw_svr_create(&raw_cfg, &raw_type, svr);
        if (svr->raw_svr == NULL) {
            http_svr_free(svr);
            return NULL;
        }
        /* on the loop of the thread the connections are on */
        nw_timer_set(&svr->continue_timer, 0, false, on_continue_timer, svr);
    }
    if (raw_cfg_out)
        *raw_cfg_out = raw_cfg;
    if (raw_type_out)
        *raw_type_out = raw_type;
    return svr;
}

/*
 * with threads. the hub calls these on each worker thread.
 */

/* a complete nw_http_svr for this thread; the hub puts the connections
 * into its nw_svr */
static nw_svr *worker_svr_create(nw_hub *hub, uint32_t index)
{
    nw_http_svr *front = hub->privdata;
    nw_http_svr *svr = http_svr_new(front->creating_cfg, &front->type, front->privdata, true, NULL, NULL);
    if (svr == NULL)
        return NULL;
    svr->hub = hub;
    svr->front = front;
    return svr->raw_svr;
}

static void worker_svr_release(nw_hub *hub, uint32_t index, nw_svr *raw_svr)
{
    http_svr_free(raw_svr->privdata);
}

static int on_thread_start(nw_hub *hub, uint32_t index)
{
    nw_http_svr *svr = nw_hub_svr(hub)->privdata;
    return svr->type.on_thread_start(svr, index);
}

static void on_thread_stop(nw_hub *hub, uint32_t index)
{
    nw_http_svr *svr = nw_hub_svr(hub)->privdata;
    svr->type.on_thread_stop(svr, index);
}

nw_http_svr *nw_http_svr_create(nw_http_svr_cfg *cfg, nw_http_svr_type *type, void *privdata)
{
    if (type->on_request == NULL)
        return NULL;
    for (uint32_t i = 0; i < cfg->bind_count; ++i) {
        if (cfg->binds[i].sock_type != SOCK_STREAM)
            return NULL;
    }
    if (cfg->thread_count == 0)
        return http_svr_new(cfg, type, privdata, true, NULL, NULL);

    /* the front server: it has the hub, whose accepting nw_svr stands in
     * for raw_svr. the workers each make their own from the same cfg */
    nw_hub_cfg hub_cfg;
    nw_hub_type hub_type;
    memset(&hub_cfg, 0, sizeof(hub_cfg));
    memset(&hub_type, 0, sizeof(hub_type));
    nw_http_svr *svr = http_svr_new(cfg, type, privdata, false, &hub_cfg.svr, &hub_type.svr);
    if (svr == NULL)
        return NULL;
    hub_cfg.thread_count = cfg->thread_count;
    hub_cfg.queue_limit = cfg->queue_limit;
    hub_type.on_svr_create = worker_svr_create;
    hub_type.on_svr_release = worker_svr_release;
    if (type->on_thread_start)
        hub_type.on_thread_start = on_thread_start;
    if (type->on_thread_stop)
        hub_type.on_thread_stop = on_thread_stop;

    svr->creating_cfg = cfg;
    svr->hub = nw_hub_create(&hub_cfg, &hub_type, svr);
    svr->creating_cfg = NULL;
    if (svr->hub == NULL) {
        http_svr_free(svr);
        return NULL;
    }
    svr->raw_svr = svr->hub->acceptor;
    return svr;
}

/* these three are for the server nw_http_svr_create returned, not for the
 * one of a worker thread */
int nw_http_svr_start(nw_http_svr *svr)
{
    if (svr->front)
        return -1;
    return svr->hub ? nw_hub_start(svr->hub) : nw_svr_start(svr->raw_svr);
}

int nw_http_svr_stop(nw_http_svr *svr)
{
    if (svr->front)
        return -1;
    return svr->hub ? nw_hub_stop(svr->hub) : nw_svr_stop(svr->raw_svr);
}

void nw_http_svr_release(nw_http_svr *svr)
{
    if (svr->front)
        return;
    if (svr->hub) {
        /* ends the workers, each of which frees its own server */
        nw_hub_release(svr->hub);
        svr->raw_svr = NULL;
    }
    http_svr_free(svr);
}
