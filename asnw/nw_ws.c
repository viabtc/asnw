/*
 * Description: websocket server (RFC 6455) on top of nw_svr
 *     History: 2026/11/25, create
 */

# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <strings.h>

# include "nw_ws.h"

# define NW_WS_DEFAULT_MAX_MESSAGE  (64 * 1024)
# define NW_WS_MAX_MESSAGE_LIMIT    (1024 * 1024 * 1024)
# define NW_WS_DEFAULT_KEEP_ALIVE   60.0
/* a client gets this long to send its upgrade request */
# define NW_WS_HANDSHAKE_TIMEOUT    10.0
/* an upgrade request is not bigger than this */
# define NW_WS_HANDSHAKE_MAX_SIZE   8192
/* a frame header: 2 bytes, up to 8 bytes of length, 4 bytes of mask */
# define NW_WS_MAX_HEADER_SIZE      14
/* a frame up to this size is put together and sent in one write, bigger
 * payloads are written separately and never copied */
# define NW_WS_SMALL_FRAME_SIZE     4096

# define NW_WS_OP_CONTINUATION      0x0
# define NW_WS_OP_TEXT              0x1
# define NW_WS_OP_BINARY            0x2
# define NW_WS_OP_CLOSE             0x8
# define NW_WS_OP_PING              0x9
# define NW_WS_OP_PONG              0xa

/*
 * the handshake needs base64(sha1(key + guid)). both are small and fixed,
 * so they live here and the library keeps libev as its only dependency.
 */

static uint32_t rol32(uint32_t value, int bits)
{
    return (value << bits) | (value >> (32 - bits));
}

static void sha1_block(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) | \
            ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t temp = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = temp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static void sha1(const void *data, size_t size, uint8_t digest[20])
{
    uint32_t state[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    const uint8_t *p = data;
    size_t left = size;
    for (; left >= 64; left -= 64, p += 64) {
        sha1_block(state, p);
    }

    /* the rest, a 0x80 byte, zeros, and the length in bits in the last 8 bytes */
    uint8_t tail[128];
    size_t tail_size = left < 56 ? 64 : 128;
    memset(tail, 0, sizeof(tail));
    memcpy(tail, p, left);
    tail[left] = 0x80;
    uint64_t bits = (uint64_t)size * 8;
    for (int i = 0; i < 8; ++i) {
        tail[tail_size - 1 - i] = (uint8_t)(bits >> (i * 8));
    }
    sha1_block(state, tail);
    if (tail_size == 128)
        sha1_block(state, tail + 64);

    for (int i = 0; i < 5; ++i) {
        digest[i * 4] = (uint8_t)(state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)state[i];
    }
}

/* out needs (size + 2) / 3 * 4 + 1 bytes */
static void base64_encode(const uint8_t *data, size_t size, char *out)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t pos = 0;
    for (size_t i = 0; i < size; i += 3) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i + 1 < size)
            n |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < size)
            n |= data[i + 2];
        out[pos++] = table[(n >> 18) & 63];
        out[pos++] = table[(n >> 12) & 63];
        out[pos++] = i + 1 < size ? table[(n >> 6) & 63] : '=';
        out[pos++] = i + 2 < size ? table[n & 63] : '=';
    }
    out[pos] = '\0';
}

/* Sec-WebSocket-Accept for a Sec-WebSocket-Key. accept needs 29 bytes */
static void accept_key(const char *key, char *accept)
{
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char input[128];
    uint8_t digest[20];
    int len = snprintf(input, sizeof(input), "%s%s", key, guid);
    sha1(input, len, digest);
    base64_encode(digest, sizeof(digest), accept);
}

/*
 * what the server keeps per connection, it is the privdata of the session
 */

typedef struct nw_ws_conn {
    /* the handshake is done, frames from here on */
    bool upgraded;
    /* handshake: bytes already searched for the end of the request */
    uint32_t scanned;
    /* the frame decode_pkg just measured, for on_recv_pkg */
    uint8_t opcode;
    bool fin;
    uint32_t header_size;
    uint32_t payload_size;
    /* why the connection has to be failed, found by decode_pkg: a http
     * status before the upgrade, a close code after it. 0 if all is well */
    int error;
    /* a message that comes in several frames is collected here */
    bool in_message;
    uint8_t message_opcode;
    char *message;
    size_t message_size;
    size_t message_total;
    /* the per connection data of the application */
    void *privdata;
} nw_ws_conn;

nw_ws_svr *nw_ws_svr_of(nw_ses *ses)
{
    return ((nw_svr *)ses->svr)->privdata;
}

void *nw_ws_privdata(nw_ses *ses)
{
    nw_ws_conn *conn = ses->privdata;
    return conn->privdata;
}

static void report(nw_ws_svr *svr, nw_ses *ses, const char *msg)
{
    if (svr->type.on_error_msg)
        svr->type.on_error_msg(ses, msg);
}

/*
 * writing
 */

/* fill in the header of an unmasked, final frame. return its size */
static size_t frame_header(uint8_t *header, uint8_t opcode, size_t size)
{
    header[0] = 0x80 | opcode;
    if (size < 126) {
        header[1] = (uint8_t)size;
        return 2;
    } else if (size <= 0xffff) {
        header[1] = 126;
        header[2] = (uint8_t)(size >> 8);
        header[3] = (uint8_t)size;
        return 4;
    }
    header[1] = 127;
    for (int i = 0; i < 8; ++i) {
        header[2 + i] = (uint8_t)((uint64_t)size >> ((7 - i) * 8));
    }
    return 10;
}

/* send one frame. return 0, or -1 if sending failed, in which case the
 * session is closed and must not be touched */
static int send_frame(nw_ses *ses, uint8_t opcode, const void *data, size_t size)
{
    uint8_t buf[NW_WS_MAX_HEADER_SIZE + NW_WS_SMALL_FRAME_SIZE];
    size_t header_size = frame_header(buf, opcode, size);
    if (size <= NW_WS_SMALL_FRAME_SIZE) {
        if (size > 0)
            memcpy(buf + header_size, data, size);
        return nw_ses_send(ses, buf, header_size + size);
    }

    nw_svr *raw_svr = ses->svr;
    uint64_t id = ses->id;
    if (nw_ses_send(ses, buf, header_size) < 0)
        return -1;
    /* go by the id: after a failed send the memory of ses may be reused */
    if (nw_svr_get_clt(raw_svr, id) != ses)
        return -1;
    return nw_ses_send(ses, data, size);
}

static bool can_send(nw_ses *ses)
{
    if (ses->ses_type != NW_SES_TYPE_COMMON || ses->id == 0)
        return false;
    nw_ws_conn *conn = ses->privdata;
    return conn->upgraded && !ses->closing;
}

int nw_ws_send_text(nw_ses *ses, const char *message)
{
    if (!can_send(ses))
        return -1;
    return send_frame(ses, NW_WS_OP_TEXT, message, strlen(message));
}

int nw_ws_send_binary(nw_ses *ses, const void *data, size_t size)
{
    if (!can_send(ses))
        return -1;
    return send_frame(ses, NW_WS_OP_BINARY, data, size);
}

/*
 * with threads (svr->hub is set) a connection belongs to one worker, which
 * is told by its id. svr may be the server of any thread, or the front
 * one. return the server of the calling thread if that is the worker of
 * id, NULL if the request has to travel.
 */
static nw_ws_svr *server_here(nw_ws_svr *svr, uint64_t id)
{
    if (svr->hub == NULL)
        return svr;
    if (nw_hub_index(svr->hub) != (int)nw_hub_index_of(id))
        return NULL;
    return nw_hub_svr(svr->hub)->privdata;
}

/* what travels to the worker of a connection: a frame to send, or a close */
struct id_request {
    uint64_t id;
    uint8_t opcode;
    uint16_t code;
    char data[];
};

/* on the worker of the connection */
static void on_id_request(nw_hub *hub, void *data, size_t size)
{
    struct id_request *request = data;
    nw_svr *raw_svr = nw_hub_svr(hub);
    nw_ses *ses = nw_svr_get_clt(raw_svr, request->id);
    if (ses == NULL)
        return;
    size_t data_size = size - sizeof(struct id_request);
    if (request->opcode == NW_WS_OP_CLOSE) {
        /* the reason was sent along with its '\0' */
        nw_ws_close(ses, request->code, data_size > 1 ? request->data : NULL);
    } else if (can_send(ses)) {
        send_frame(ses, request->opcode, request->data, data_size);
    }
}

static int post_id_request(nw_ws_svr *svr, uint64_t id, uint8_t opcode, uint16_t code, const void *data, size_t size)
{
    if (id == 0)
        return -1;
    struct id_request *request = malloc(sizeof(struct id_request) + size);
    if (request == NULL)
        return -1;
    memset(request, 0, sizeof(struct id_request));
    request->id = id;
    request->opcode = opcode;
    request->code = code;
    if (size > 0)
        memcpy(request->data, data, size);
    int ret = nw_hub_post(svr->hub, nw_hub_index_of(id), on_id_request, request, sizeof(struct id_request) + size);
    free(request);
    return ret;
}

static int send_id(nw_ws_svr *svr, uint64_t id, uint8_t opcode, const void *data, size_t size)
{
    nw_ws_svr *here = server_here(svr, id);
    if (here == NULL)
        return post_id_request(svr, id, opcode, 0, data, size);
    nw_ses *ses = nw_svr_get_clt(here->raw_svr, id);
    if (ses == NULL || !can_send(ses))
        return -1;
    return send_frame(ses, opcode, data, size);
}

int nw_ws_send_text_id(nw_ws_svr *svr, uint64_t id, const char *message)
{
    return send_id(svr, id, NW_WS_OP_TEXT, message, strlen(message));
}

int nw_ws_send_binary_id(nw_ws_svr *svr, uint64_t id, const void *data, size_t size)
{
    return send_id(svr, id, NW_WS_OP_BINARY, data, size);
}

/* send a finished frame to every connection of one nw_svr that can take it */
static int broadcast_frame(nw_svr *raw_svr, const void *frame, size_t frame_size)
{
    int count = 0;
    nw_svr_iterator iter;
    nw_ses *ses;
    nw_svr_iter_init(&iter, raw_svr);
    /* a send that fails closes that connection, which the iterator allows,
     * and the others still get their copy */
    while ((ses = nw_svr_next(&iter)) != NULL) {
        if (can_send(ses) && nw_ses_send(ses, frame, frame_size) == 0)
            count++;
    }
    return count;
}

/* on every worker */
static void on_broadcast(nw_hub *hub, void *data, size_t size)
{
    broadcast_frame(nw_hub_svr(hub), data, size);
}

static int broadcast(nw_ws_svr *svr, uint8_t opcode, const void *data, size_t size)
{
    /* the frame is the same for everybody: it is put together once, here,
     * and the same bytes go to every connection, on every thread */
    uint8_t small[NW_WS_MAX_HEADER_SIZE + NW_WS_SMALL_FRAME_SIZE];
    uint8_t *frame = small;
    if (size > NW_WS_SMALL_FRAME_SIZE) {
        frame = malloc(NW_WS_MAX_HEADER_SIZE + size);
        if (frame == NULL)
            return svr->hub ? -1 : 0;
    }
    size_t frame_size = frame_header(frame, opcode, size);
    if (size > 0)
        memcpy(frame + frame_size, data, size);
    frame_size += size;

    int ret;
    if (svr->hub) {
        ret = nw_hub_post_all(svr->hub, on_broadcast, frame, frame_size);
    } else {
        ret = broadcast_frame(svr->raw_svr, frame, frame_size);
    }
    if (frame != small)
        free(frame);
    return ret;
}

int nw_ws_broadcast_text(nw_ws_svr *svr, const char *message)
{
    return broadcast(svr, NW_WS_OP_TEXT, message, strlen(message));
}

int nw_ws_broadcast_binary(nw_ws_svr *svr, const void *data, size_t size)
{
    return broadcast(svr, NW_WS_OP_BINARY, data, size);
}

/* send a close frame, then wait for the client to hang up */
static void ws_close(nw_ses *ses, nw_ws_conn *conn, uint16_t code, const char *reason)
{
    uint8_t payload[125];
    size_t reason_size = reason ? strlen(reason) : 0;
    if (reason_size > sizeof(payload) - 2)
        reason_size = sizeof(payload) - 2;
    payload[0] = (uint8_t)(code >> 8);
    payload[1] = (uint8_t)code;
    if (reason_size > 0)
        memcpy(payload + 2, reason, reason_size);

    nw_svr *raw_svr = ses->svr;
    uint64_t id = ses->id;
    if (send_frame(ses, NW_WS_OP_CLOSE, payload, 2 + reason_size) < 0)
        return;
    if (nw_svr_get_clt(raw_svr, id) != ses)
        return;
    /* nothing is sent after the close frame, what the client still sends
     * is ignored, and the connection ends when it hangs up */
    nw_svr_shutdown_clt(raw_svr, ses);
}

void nw_ws_close(nw_ses *ses, uint16_t code, const char *reason)
{
    if (ses->ses_type != NW_SES_TYPE_COMMON || ses->id == 0)
        return;
    nw_ws_conn *conn = ses->privdata;
    if (ses->closing)
        return;
    if (!conn->upgraded) {
        nw_svr_close_clt(ses->svr, ses);
        return;
    }
    ws_close(ses, conn, code, reason);
}

/* the client broke the protocol: tell it why, then close */
static void ws_fail(nw_ws_svr *svr, nw_ses *ses, nw_ws_conn *conn, int code)
{
    char msg[64];
    snprintf(msg, sizeof(msg), "connection failed with close code %d", code);
    report(svr, ses, msg);
    ws_close(ses, conn, (uint16_t)code, NULL);
}

/*
 * handshake
 */

/* answer an upgrade request that is not accepted with a plain http error */
static void refuse_upgrade(nw_ws_svr *svr, nw_ses *ses, nw_ws_conn *conn, int status)
{
    char body[64];
    char response[256];
    int body_size = snprintf(body, sizeof(body), "%d %s\n", status, nw_http_status_text(status));
    int size = snprintf(response, sizeof(response),
            "HTTP/1.1 %d %s\r\n"
            "%s"
            "Content-Type: text/plain\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            status, nw_http_status_text(status),
            status == 426 ? "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n" : "",
            body_size, body);

    char msg[100];
    snprintf(msg, sizeof(msg), "upgrade refused: %d %s", status, nw_http_status_text(status));
    report(svr, ses, msg);

    nw_svr *raw_svr = ses->svr;
    uint64_t id = ses->id;
    if (nw_ses_send(ses, response, size) < 0 || nw_svr_get_clt(raw_svr, id) != ses)
        return;
    nw_svr_shutdown_clt(raw_svr, ses);
}

/* does a comma separated header value hold this token, case insensitive */
static bool has_token(const char *value, const char *token)
{
    size_t token_size = strlen(token);
    const char *pos = value;
    const char *end = value + strlen(value);
    while (pos < end) {
        const char *comma = memchr(pos, ',', end - pos);
        const char *item = pos;
        const char *item_end = comma ? comma : end;
        while (item < item_end && (*item == ' ' || *item == '\t'))
            item++;
        while (item_end > item && (item_end[-1] == ' ' || item_end[-1] == '\t'))
            item_end--;
        if ((size_t)(item_end - item) == token_size && strncasecmp(item, token, token_size) == 0)
            return true;
        pos = comma ? comma + 1 : end;
    }
    return false;
}

/*
 * is this Origin header acceptable. allowed is a host name ("example.com":
 * that host and its subdomains, any scheme and port) or a full origin with
 * a scheme, which has to match exactly.
 */
static bool origin_allowed(const char *origin, const char *allowed)
{
    if (strstr(allowed, "://") != NULL)
        return strcasecmp(origin, allowed) == 0;

    const char *host = strstr(origin, "://");
    if (host == NULL)
        return false;
    host += 3;
    size_t host_size = strcspn(host, ":/");
    size_t allowed_size = strlen(allowed);
    if (host_size == allowed_size)
        return strncasecmp(host, allowed, allowed_size) == 0;
    /* a subdomain: the character in front of the suffix has to be the dot,
     * or "evilexample.com" would pass for "example.com" */
    if (host_size > allowed_size && host[host_size - allowed_size - 1] == '.')
        return strncasecmp(host + host_size - allowed_size, allowed, allowed_size) == 0;
    return false;
}

/* the status an upgrade request has to be refused with, 0 if it is fine */
static int check_upgrade(nw_ws_svr *svr, nw_http_request *req, bool *offered_protocol)
{
    if (strcmp(req->method, "GET") != 0 || req->version_minor < 1 || req->body_size != 0)
        return 400;
    const char *upgrade = nw_http_get_header(req, "Upgrade");
    const char *connection = nw_http_get_header(req, "Connection");
    if (upgrade == NULL || connection == NULL)
        return 426;
    if (!has_token(upgrade, "websocket") || !has_token(connection, "upgrade"))
        return 426;
    const char *version = nw_http_get_header(req, "Sec-WebSocket-Version");
    if (version == NULL || strcmp(version, "13") != 0)
        return 426;
    /* 16 random bytes in base64 */
    const char *key = nw_http_get_header(req, "Sec-WebSocket-Key");
    if (key == NULL || strlen(key) != 24)
        return 400;

    const char *protocols = nw_http_get_header(req, "Sec-WebSocket-Protocol");
    *offered_protocol = false;
    if (protocols != NULL && svr->protocol != NULL) {
        if (!has_token(protocols, svr->protocol))
            return 400;
        *offered_protocol = true;
    }
    if (svr->origin != NULL) {
        const char *origin = nw_http_get_header(req, "Origin");
        if (origin == NULL || !origin_allowed(origin, svr->origin))
            return 403;
    }
    return 0;
}

static void on_handshake(nw_ws_svr *svr, nw_ses *ses, nw_ws_conn *conn, char *data, size_t size)
{
    nw_http_request req;
    int status = nw_http_parse_request(data, size, &req);
    bool offered_protocol = false;
    if (status == 0)
        status = check_upgrade(svr, &req, &offered_protocol);
    if (status != 0) {
        refuse_upgrade(svr, ses, conn, status);
        return;
    }

    if (svr->type.on_privdata_alloc) {
        conn->privdata = svr->type.on_privdata_alloc(svr);
        if (conn->privdata == NULL) {
            refuse_upgrade(svr, ses, conn, 500);
            return;
        }
    }
    if (svr->type.on_upgrade && svr->type.on_upgrade(ses, &req) < 0) {
        if (conn->privdata) {
            svr->type.on_privdata_free(svr, conn->privdata);
            conn->privdata = NULL;
        }
        refuse_upgrade(svr, ses, conn, 403);
        return;
    }

    char accept[32];
    char response[512];
    accept_key(nw_http_get_header(&req, "Sec-WebSocket-Key"), accept);
    int len = snprintf(response, sizeof(response),
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n",
            accept);
    if (offered_protocol)
        len += snprintf(response + len, sizeof(response) - len, "Sec-WebSocket-Protocol: %.128s\r\n", svr->protocol);
    len += snprintf(response + len, sizeof(response) - len, "\r\n");

    /* upgraded before sending: if the send fails the connection is closed
     * right there, and on_close and on_privdata_free have to run for it */
    nw_svr *raw_svr = ses->svr;
    uint64_t id = ses->id;
    conn->upgraded = true;
    /* the handshake made its deadline. from here on the idle timeout paces
     * the pings, or is the keep-alive timeout itself */
    nw_svr_set_clt_deadline(raw_svr, ses, 0);
    nw_svr_set_clt_idle_timeout(raw_svr, ses, svr->ping_interval > 0 ? svr->ping_interval : svr->keep_alive_timeout);
    if (nw_ses_send(ses, response, len) < 0 || nw_svr_get_clt(raw_svr, id) != ses)
        return;
    if (svr->type.on_open)
        svr->type.on_open(ses);
}

/*
 * reading
 */

static int decode_pkg(nw_ses *ses, void *data, size_t max)
{
    nw_ws_svr *svr = nw_ws_svr_of(ses);
    nw_ws_conn *conn = ses->privdata;
    uint8_t *p = data;

    if (conn->error)
        return max;

    if (!conn->upgraded) {
        /* the upgrade request ends with an empty line. resume the search
         * where the last call stopped */
        size_t pos = conn->scanned > 3 ? conn->scanned - 3 : 0;
        for (; pos + 4 <= max; ++pos) {
            uint8_t *cr = memchr(p + pos, '\r', max - pos - 3);
            if (cr == NULL)
                break;
            pos = cr - p;
            if (p[pos + 1] == '\n' && p[pos + 2] == '\r' && p[pos + 3] == '\n') {
                if (pos + 4 > NW_WS_HANDSHAKE_MAX_SIZE)
                    break;
                return pos + 4;
            }
        }
        if (max >= NW_WS_HANDSHAKE_MAX_SIZE || max >= ses->read_buf->size) {
            /* decode_pkg must not send: on_recv_pkg does the refusing */
            conn->error = 431;
            return max;
        }
        conn->scanned = max;
        return 0;
    }

    if (max < 2)
        return 0;
    conn->fin = (p[0] & 0x80) != 0;
    conn->opcode = p[0] & 0x0f;
    bool control = conn->opcode >= NW_WS_OP_CLOSE;
    uint64_t payload_size = p[1] & 0x7f;
    uint32_t header_size = 2;

    /* reserved bits are for extensions and none is negotiated; a client
     * always masks; a control frame is short and never split */
    if ((p[0] & 0x70) != 0 || (p[1] & 0x80) == 0)
        goto protocol_error;
    if (conn->opcode > NW_WS_OP_BINARY && conn->opcode < NW_WS_OP_CLOSE)
        goto protocol_error;
    if (conn->opcode > NW_WS_OP_PONG)
        goto protocol_error;
    if (control && (!conn->fin || payload_size > 125))
        goto protocol_error;

    if (payload_size == 126) {
        if (max < 4)
            return 0;
        payload_size = ((uint64_t)p[2] << 8) | p[3];
        header_size = 4;
    } else if (payload_size == 127) {
        if (max < 10)
            return 0;
        payload_size = 0;
        for (int i = 0; i < 8; ++i)
            payload_size = (payload_size << 8) | p[2 + i];
        header_size = 10;
    }
    /* check the length the client claims before doing arithmetic with it:
     * it can be anything up to 2^64 */
    if (payload_size > svr->max_message_size) {
        conn->error = NW_WS_CLOSE_TOO_BIG;
        return max;
    }
    header_size += 4;
    conn->header_size = header_size;
    conn->payload_size = (uint32_t)payload_size;
    size_t total = header_size + (size_t)payload_size;
    return max >= total ? (int)total : 0;

protocol_error:
    conn->error = NW_WS_CLOSE_PROTOCOL_ERROR;
    return max;
}

/* hand a complete message to the application. the connection may be gone
 * when this returns */
static void deliver(nw_ws_svr *svr, nw_ses *ses, uint8_t opcode, char *data, size_t size)
{
    svr->type.on_message(ses, opcode == NW_WS_OP_BINARY, data, size);
}

static void on_frame(nw_ws_svr *svr, nw_ses *ses, nw_ws_conn *conn, uint8_t *frame)
{
    /*
     * unmask. the 4 mask bytes sit right in front of the payload and are
     * not needed afterwards, so the payload is moved over them while it is
     * unmasked anyway. that frees room behind it for a '\0', without
     * touching the next frame, and costs nothing extra.
     */
    size_t size = conn->payload_size;
    uint8_t *in = frame + conn->header_size;
    uint8_t *out = in - 4;
    uint8_t mask[4];
    memcpy(mask, out, 4);
    for (size_t i = 0; i < size; ++i) {
        out[i] = in[i] ^ mask[i & 3];
    }
    out[size] = '\0';
    char *payload = (char *)out;

    switch (conn->opcode) {
    case NW_WS_OP_PING:
        /* the pong carries what the ping carried */
        send_frame(ses, NW_WS_OP_PONG, payload, size);
        return;
    case NW_WS_OP_PONG:
        return;
    case NW_WS_OP_CLOSE:
        {
            /* answer with the same code, then both sides are done */
            uint16_t code = NW_WS_CLOSE_NORMAL;
            if (size == 1) {
                ws_fail(svr, ses, conn, NW_WS_CLOSE_PROTOCOL_ERROR);
                return;
            }
            if (size >= 2)
                code = (uint16_t)(((uint8_t)payload[0] << 8) | (uint8_t)payload[1]);
            ws_close(ses, conn, code, NULL);
        }
        return;
    case NW_WS_OP_TEXT:
    case NW_WS_OP_BINARY:
        if (conn->in_message) {
            /* a new message while one is still being put together */
            ws_fail(svr, ses, conn, NW_WS_CLOSE_PROTOCOL_ERROR);
            return;
        }
        if (conn->fin) {
            /* the usual case: one frame, delivered where it lies */
            deliver(svr, ses, conn->opcode, payload, size);
            return;
        }
        conn->in_message = true;
        conn->message_opcode = conn->opcode;
        conn->message_size = 0;
        break;
    default:
        if (!conn->in_message) {
            /* a continuation of nothing */
            ws_fail(svr, ses, conn, NW_WS_CLOSE_PROTOCOL_ERROR);
            return;
        }
        break;
    }

    /* one piece of a message that comes in several frames */
    if (conn->message_size + size > svr->max_message_size) {
        ws_fail(svr, ses, conn, NW_WS_CLOSE_TOO_BIG);
        return;
    }
    if (conn->message_size + size + 1 > conn->message_total) {
        size_t new_total = conn->message_total ? conn->message_total : 1024;
        while (new_total < conn->message_size + size + 1)
            new_total *= 2;
        char *new_message = realloc(conn->message, new_total);
        if (new_message == NULL) {
            ws_fail(svr, ses, conn, NW_WS_CLOSE_SERVER_ERROR);
            return;
        }
        conn->message = new_message;
        conn->message_total = new_total;
    }
    memcpy(conn->message + conn->message_size, payload, size);
    conn->message_size += size;
    if (!conn->fin)
        return;

    /* complete. the buffer is taken away from the connection first: the
     * callback may close it */
    char *message = conn->message;
    size_t message_size = conn->message_size;
    uint8_t message_opcode = conn->message_opcode;
    conn->message = NULL;
    conn->message_size = 0;
    conn->message_total = 0;
    conn->in_message = false;
    message[message_size] = '\0';
    deliver(svr, ses, message_opcode, message, message_size);
    free(message);
}

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    nw_ws_svr *svr = nw_ws_svr_of(ses);
    nw_ws_conn *conn = ses->privdata;

    if (!conn->upgraded) {
        conn->scanned = 0;
        if (conn->error) {
            int status = conn->error;
            conn->error = 0;
            refuse_upgrade(svr, ses, conn, status);
            return;
        }
        on_handshake(svr, ses, conn, data, size);
        return;
    }

    if (conn->error) {
        int code = conn->error;
        conn->error = 0;
        ws_fail(svr, ses, conn, code);
        return;
    }
    on_frame(svr, ses, conn, data);
}

/*
 * server
 */

static void on_new_connection(nw_ses *ses)
{
    /* the upgrade request has to be here in time */
    nw_svr_set_clt_deadline(ses->svr, ses, NW_WS_HANDSHAKE_TIMEOUT);
}

static void on_connection_close(nw_ses *ses)
{
    nw_ws_svr *svr = nw_ws_svr_of(ses);
    nw_ws_conn *conn = ses->privdata;
    if (conn->upgraded && svr->type.on_close)
        svr->type.on_close(ses);
    if (conn->privdata) {
        svr->type.on_privdata_free(svr, conn->privdata);
        conn->privdata = NULL;
    }
    free(conn->message);
    conn->message = NULL;
}

int nw_ws_close_id(nw_ws_svr *svr, uint64_t id, uint16_t code, const char *reason)
{
    nw_ws_svr *here = server_here(svr, id);
    if (here == NULL)
        return post_id_request(svr, id, NW_WS_OP_CLOSE, code, reason, reason ? strlen(reason) + 1 : 0);
    nw_ses *ses = nw_svr_get_clt(here->raw_svr, id);
    if (ses)
        nw_ws_close(ses, code, reason);
    return 0;
}

static void on_error_msg(nw_ses *ses, const char *msg)
{
    report(nw_ws_svr_of(ses), ses, msg);
}

static void on_drain(nw_ses *ses)
{
    nw_ws_svr *svr = nw_ws_svr_of(ses);
    nw_ws_conn *conn = ses->privdata;
    if (conn->upgraded && svr->type.on_drain)
        svr->type.on_drain(ses);
}

static void *on_privdata_alloc(void *raw_svr)
{
    nw_ws_svr *svr = ((nw_svr *)raw_svr)->privdata;
    nw_ws_conn *conn = nw_cache_alloc(svr->conn_cache);
    if (conn)
        memset(conn, 0, sizeof(nw_ws_conn));
    return conn;
}

static void on_privdata_free(void *raw_svr, void *privdata)
{
    nw_ws_svr *svr = ((nw_svr *)raw_svr)->privdata;
    nw_cache_free(svr->conn_cache, privdata);
}

/* the handshake deadline has passed, or the connection has been silent for
 * another ping interval (or, without pings, for the keep-alive timeout) */
static void on_timeout(nw_ses *ses)
{
    nw_ws_svr *svr = nw_ws_svr_of(ses);
    nw_ws_conn *conn = ses->privdata;
    if (!conn->upgraded) {
        nw_svr_close_clt(svr->raw_svr, ses);
        return;
    }
    double silence = ev_now(nw_default_loop) - ses->active_time;
    if (silence >= svr->keep_alive_timeout) {
        report(svr, ses, "idle too long");
        ws_close(ses, conn, NW_WS_CLOSE_GOING_AWAY, "idle timeout");
    } else {
        /* a live client answers with a pong, which ends the silence */
        send_frame(ses, NW_WS_OP_PING, NULL, 0);
    }
}

static void ws_svr_free(nw_ws_svr *svr)
{
    if (svr->raw_svr)
        nw_svr_release(svr->raw_svr);
    if (svr->conn_cache)
        nw_cache_release(svr->conn_cache);
    free(svr->protocol);
    free(svr->origin);
    free(svr);
}

/*
 * the settings, and what the nw_svr underneath is made from. with threads
 * this runs once for the front server (with_svr false: its nw_svr is the
 * accepting one of the hub) and once on every worker thread.
 */
static nw_ws_svr *ws_svr_new(nw_ws_svr_cfg *cfg, nw_ws_svr_type *type, void *privdata, bool with_svr,
        nw_svr_cfg *raw_cfg_out, nw_svr_type *raw_type_out)
{
    nw_ws_svr *svr = malloc(sizeof(nw_ws_svr));
    if (svr == NULL)
        return NULL;
    memset(svr, 0, sizeof(nw_ws_svr));
    svr->type = *type;
    svr->privdata = privdata;
    svr->max_message_size = cfg->max_message_size ? cfg->max_message_size : NW_WS_DEFAULT_MAX_MESSAGE;
    if (svr->max_message_size > NW_WS_MAX_MESSAGE_LIMIT)
        svr->max_message_size = NW_WS_MAX_MESSAGE_LIMIT;
    svr->keep_alive_timeout = cfg->keep_alive_timeout > 0 ? cfg->keep_alive_timeout : NW_WS_DEFAULT_KEEP_ALIVE;
    svr->ping_interval = cfg->ping_interval > 0 ? cfg->ping_interval : 0;
    if (cfg->protocol && cfg->protocol[0])
        svr->protocol = strdup(cfg->protocol);
    if (cfg->origin && cfg->origin[0])
        svr->origin = strdup(cfg->origin);
    svr->conn_cache = nw_cache_create(sizeof(nw_ws_conn));
    if (svr->conn_cache == NULL || (cfg->protocol && cfg->protocol[0] && svr->protocol == NULL) || \
            (cfg->origin && cfg->origin[0] && svr->origin == NULL)) {
        ws_svr_free(svr);
        return NULL;
    }

    nw_svr_cfg raw_cfg;
    memset(&raw_cfg, 0, sizeof(raw_cfg));
    raw_cfg.bind_count = cfg->bind_count;
    raw_cfg.bind_arr = cfg->bind_arr;
    /* room for the biggest frame, and at least for an upgrade request */
    raw_cfg.max_pkg_size = svr->max_message_size + NW_WS_MAX_HEADER_SIZE;
    if (raw_cfg.max_pkg_size < NW_WS_HANDSHAKE_MAX_SIZE)
        raw_cfg.max_pkg_size = NW_WS_HANDSHAKE_MAX_SIZE;
    raw_cfg.buf_limit = cfg->buf_limit;
    raw_cfg.read_mem = cfg->read_mem;
    raw_cfg.write_mem = cfg->write_mem;

    nw_svr_type raw_type;
    memset(&raw_type, 0, sizeof(raw_type));
    raw_type.decode_pkg = decode_pkg;
    raw_type.on_recv_pkg = on_recv_pkg;
    raw_type.on_new_connection = on_new_connection;
    raw_type.on_connection_close = on_connection_close;
    raw_type.on_error_msg = on_error_msg;
    raw_type.on_timeout = on_timeout;
    raw_type.on_drain = on_drain;
    raw_type.on_privdata_alloc = on_privdata_alloc;
    raw_type.on_privdata_free = on_privdata_free;

    if (with_svr) {
        svr->raw_svr = nw_svr_create(&raw_cfg, &raw_type, svr);
        if (svr->raw_svr == NULL) {
            ws_svr_free(svr);
            return NULL;
        }
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

/* a complete nw_ws_svr for this thread; the hub puts the connections into
 * its nw_svr */
static nw_svr *worker_svr_create(nw_hub *hub, uint32_t index)
{
    nw_ws_svr *front = hub->privdata;
    nw_ws_svr *svr = ws_svr_new(front->creating_cfg, &front->type, front->privdata, true, NULL, NULL);
    if (svr == NULL)
        return NULL;
    svr->hub = hub;
    svr->front = front;
    return svr->raw_svr;
}

static void worker_svr_release(nw_hub *hub, uint32_t index, nw_svr *raw_svr)
{
    ws_svr_free(raw_svr->privdata);
}

static int on_thread_start(nw_hub *hub, uint32_t index)
{
    nw_ws_svr *svr = nw_hub_svr(hub)->privdata;
    return svr->type.on_thread_start(svr, index);
}

static void on_thread_stop(nw_hub *hub, uint32_t index)
{
    nw_ws_svr *svr = nw_hub_svr(hub)->privdata;
    svr->type.on_thread_stop(svr, index);
}

nw_ws_svr *nw_ws_svr_create(nw_ws_svr_cfg *cfg, nw_ws_svr_type *type, void *privdata)
{
    if (type->on_message == NULL)
        return NULL;
    if (type->on_privdata_alloc && !type->on_privdata_free)
        return NULL;
    for (uint32_t i = 0; i < cfg->bind_count; ++i) {
        if (cfg->bind_arr[i].sock_type != SOCK_STREAM)
            return NULL;
    }
    if (cfg->thread_count == 0)
        return ws_svr_new(cfg, type, privdata, true, NULL, NULL);

    /* the front server: it has the hub, whose accepting nw_svr stands in
     * for raw_svr. the workers each make their own from the same cfg */
    nw_hub_cfg hub_cfg;
    nw_hub_type hub_type;
    memset(&hub_cfg, 0, sizeof(hub_cfg));
    memset(&hub_type, 0, sizeof(hub_type));
    nw_ws_svr *svr = ws_svr_new(cfg, type, privdata, false, &hub_cfg.svr, &hub_type.svr);
    if (svr == NULL)
        return NULL;
    hub_cfg.thread_count = cfg->thread_count;
    hub_cfg.queue_limit = cfg->queue_limit;
    hub_type.svr_create = worker_svr_create;
    hub_type.svr_release = worker_svr_release;
    if (type->on_thread_start)
        hub_type.on_thread_start = on_thread_start;
    if (type->on_thread_stop)
        hub_type.on_thread_stop = on_thread_stop;

    svr->creating_cfg = cfg;
    svr->hub = nw_hub_create(&hub_cfg, &hub_type, svr);
    svr->creating_cfg = NULL;
    if (svr->hub == NULL) {
        ws_svr_free(svr);
        return NULL;
    }
    svr->raw_svr = svr->hub->acceptor;
    return svr;
}

/* these three are for the server nw_ws_svr_create returned, not for the
 * one of a worker thread */
int nw_ws_svr_start(nw_ws_svr *svr)
{
    if (svr->front)
        return -1;
    return svr->hub ? nw_hub_start(svr->hub) : nw_svr_start(svr->raw_svr);
}

int nw_ws_svr_stop(nw_ws_svr *svr)
{
    if (svr->front)
        return -1;
    return svr->hub ? nw_hub_stop(svr->hub) : nw_svr_stop(svr->raw_svr);
}

void nw_ws_svr_release(nw_ws_svr *svr)
{
    if (svr->front)
        return;
    if (svr->hub) {
        /* ends the workers, each of which frees its own server */
        nw_hub_release(svr->hub);
        svr->raw_svr = NULL;
    }
    ws_svr_free(svr);
}
