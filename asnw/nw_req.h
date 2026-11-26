/*
 * Description: a small http client: GET or POST without blocking the loop
 *     History: yang@haipo.me, 2026/11/26, create
 */

# ifndef _NW_REQ_H_
# define _NW_REQ_H_

# include <stdint.h>
# include <stddef.h>

/*
 * nw_req makes a http request for a program that cannot wait for it: the
 * request is made on a thread of its own with plain blocking code (look
 * the name up, connect, send, read), and the response is handed to a
 * callback on the loop thread. it is meant for the occasional request to
 * a service of the same trusted network, such as reading a configuration
 * every minute or posting an alert, not for talking to another service
 * thousands of times a second.
 *
 *     static void on_config(int error, nw_req_rsp *rsp, void *privdata)
 *     {
 *         if (error)
 *             log("config: %s", rsp->message);
 *         else if (rsp->status != 200)
 *             log("config: status %d", rsp->status);
 *         else
 *             load_config(rsp->body, rsp->body_size);
 *     }
 *
 *     nw_req_get("http://config.internal:8500/v1/app?env=prod", 3.0, on_config, NULL);
 *
 * what it does: GET and POST over http, a server given by name or by
 * address, every address of a name tried in turn, responses with a
 * Content-Length, chunked ones, and those that end with the connection.
 *
 * what it does not: https, other methods, request headers of your own,
 * redirects (a 301 is handed to the callback as it is), compression,
 * keeping connections for the next request, trying again.
 */

/* threads making the requests, for each loop thread that uses nw_req.
 * that many requests are on their way at a time, the others wait for
 * their turn. the threads are started by the first request and stopped
 * by nw_loop_release */
# define NW_REQ_THREADS 2
/* a response bigger than this, head included, is refused, and so is the
 * body of a POST */
# define NW_REQ_MAX_SIZE (16 * 1024 * 1024)
# define NW_REQ_URL_MAX 4096
# define NW_REQ_MAX_HEADERS 64
/* the timeout when 0 is given, in seconds */
# define NW_REQ_TIMEOUT 10.0

enum {
    NW_REQ_ERR_RESOLVE = 1,     /* the name of the server was not found */
    NW_REQ_ERR_CONNECT,         /* no address of the server took the connection */
    NW_REQ_ERR_TIMEOUT,         /* no complete response in time */
    NW_REQ_ERR_IO,              /* the connection broke, or ended too early */
    NW_REQ_ERR_RESPONSE,        /* what came back is not a http response */
    NW_REQ_ERR_TOO_LARGE,       /* more than NW_REQ_MAX_SIZE */
    NW_REQ_ERR_MEMORY,
};

typedef struct nw_req_header {
    char *name;
    char *value;
} nw_req_header;

typedef struct nw_req_rsp {
    /* 200, 404 ... a status other than 200 is not an error: check it */
    int status;
    /* as sent by the server, see nw_req_get_header */
    uint32_t header_count;
    nw_req_header headers[NW_REQ_MAX_HEADERS];
    /* never NULL, and followed by a '\0' that body_size does not count */
    char *body;
    size_t body_size;
    /* when error is set: what went wrong, for the log. "" otherwise */
    const char *message;
} nw_req_rsp;

/*
 * called on the loop thread, once for each request that was not cancelled.
 * error is 0 when a response was received, whatever its status, or one of
 * NW_REQ_ERR_...: then rsp only holds a message. rsp and all it points to
 * are gone when the callback returns: copy what you keep.
 */
typedef void (*nw_req_callback)(int error, nw_req_rsp *rsp, void *privdata);

typedef struct nw_req nw_req;

/*
 * GET a url: "http://host[:port][/path][?query]". host is a name, an ipv4
 * address or an ipv6 address in brackets.
 *
 * timeout, in seconds, is for the whole request, the wait for a free
 * thread included; 0 means NW_REQ_TIMEOUT. one thing may outlast it: the
 * lookup of a name cannot be interrupted, so with a dns server that does
 * not answer the error comes when the system resolver gives up.
 *
 * return NULL if the url is not one of the above (https for instance) or
 * if out of memory: then the callback is not called. the return value is
 * only good for nw_req_cancel, until the callback has been called.
 *
 * callback may be NULL, for a request whose outcome does not matter: it
 * is made all the same, and nothing tells whether it worked. the return
 * value then only says that the request was accepted, and must not be
 * given to nw_req_cancel, as nothing tells when the request is over.
 *
 * the url is sent as it is, so what is put into it has to be encoded
 * first, see nw_req_url_encode.
 */
nw_req *nw_req_get(const char *url, double timeout, nw_req_callback callback, void *privdata);

/*
 * POST size bytes of body to a url. content_type is what the body is,
 * "text/plain" for instance; NULL means "application/json". the body is
 * copied: it need not outlive the call. a body of no bytes is fine.
 *
 * everything else is as for nw_req_get, NULL also being returned for a
 * body larger than NW_REQ_MAX_SIZE. one thing to keep in mind: a request
 * that failed may have been received. after a timeout or a broken
 * connection the server has perhaps done what was asked, so nw_req never
 * sends a request twice, and whether to try again is yours to decide.
 */
nw_req *nw_req_post(const char *url, const char *content_type, const void *body, size_t size,
        double timeout, nw_req_callback callback, void *privdata);

/* forget a request: its callback will not be called. not to be called
 * once the callback has been, nor from inside it. requests still on their
 * way when the loop is released are dropped the same way */
void nw_req_cancel(nw_req *req);

/*
 * encode size bytes of anything so that they can be part of a url: the
 * value of a parameter, a piece of a path. letters, digits and - _ . ~
 * stay as they are, every other byte becomes %XX, a space %20.
 *
 *     char name[256], url[512];
 *     nw_req_url_encode(user_name, strlen(user_name), name, sizeof(name));
 *     snprintf(url, sizeof(url), "http://users.internal/find?name=%s&limit=10", name);
 *
 * encode the values, not the whole url: its / ? & = would be encoded too.
 * dest gets a '\0' at the end and needs up to size * 3 + 1 bytes.
 * return the length of the result, or -1 if dest_size is too small.
 */
int nw_req_url_encode(const void *data, size_t size, char *dest, size_t dest_size);

/* the value of a header of the response, whatever the case of its name,
 * or NULL */
const char *nw_req_get_header(nw_req_rsp *rsp, const char *name);

# endif
