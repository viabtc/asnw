/*
 * Description: client, keeps a connection to one address
 *     History: yang@haipo.me, 2016/03/22, create
 */

# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <errno.h>
# include <time.h>
# include <unistd.h>

# include "nw_clt.h"

static int create_socket(int family, int sock_type)
{
    int sockfd = socket(family, sock_type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sockfd < 0) {
        return -1;
    }
    if (sock_type == SOCK_STREAM && (family == AF_INET || family == AF_INET6)) {
        if (nw_sock_set_no_delay(sockfd) < 0) {
            close(sockfd);
            return -1;
        }
    }

    return sockfd;
}

/* best effort: a refused buffer size is not a reason to drop the connection */
static void set_socket_option(nw_clt *clt, int sockfd)
{
    if (clt->read_mem > 0) {
        nw_sock_set_recv_buf(sockfd, clt->read_mem);
    }
    if (clt->write_mem > 0) {
        nw_sock_set_send_buf(sockfd, clt->write_mem);
    }
}

/* local address of a unix dgram client, unique per process and per start,
 * whichever thread asks. does not touch the application's rand() state */
static void generate_local_path(char *path, size_t size)
{
    static unsigned int counter;
    unsigned int seq = __sync_fetch_and_add(&counter, 1);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(path, size, "%s/asnw_dgram_%d_%u_%09ld.sock", P_tmpdir, (int)getpid(), seq, (long)ts.tv_nsec);
}

static void reconnect_later(nw_clt *clt, double delay);

static void on_reconnect_timeout(nw_timer *timer, void *privdata)
{
    nw_clt *clt = (nw_clt *)privdata;
    if (nw_clt_start(clt) < 0) {
        /* could not even create a socket (out of fds ...), keep trying */
        reconnect_later(clt, clt->reconnect_timeout);
    }
}

static void reconnect_later(nw_clt *clt, double delay)
{
    nw_timer_stop(&clt->timer);
    nw_timer_set(&clt->timer, delay, false, on_reconnect_timeout, clt);
    nw_timer_start(&clt->timer);
}

static int call_on_close(nw_clt *clt);
static int clt_close(nw_clt *clt);
static bool try_next_addr(nw_clt *clt);

/* the connect attempt got no answer in time: give it up and try again */
static void on_connect_timeout(nw_timer *timer, void *privdata)
{
    nw_clt *clt = (nw_clt *)privdata;
    if (clt->on_connect_called)
        return;
    if (try_next_addr(clt))
        return;
    call_on_close(clt);
    clt_close(clt);
    if (clt->close_requested) {
        clt->close_requested = false;
        return;
    }
    if (nw_clt_start(clt) < 0)
        reconnect_later(clt, clt->reconnect_timeout);
}

static void watch_connect(nw_clt *clt)
{
    nw_timer_stop(&clt->timer);
    nw_timer_set(&clt->timer, clt->reconnect_timeout, false, on_connect_timeout, clt);
    nw_timer_start(&clt->timer);
}

static void on_recv_fd(nw_ses *ses, int fd)
{
    close(fd);
}

static int clt_close(nw_clt *clt)
{
    nw_timer_stop(&clt->timer);
    clt->connected = false;
    if (clt->dns_req) {
        /* its answer is not wanted any more */
        nw_dns_cancel(clt->dns_req);
        clt->dns_req = NULL;
    }
    int ret = nw_ses_close(&clt->ses);
    /* remove the socket file a unix dgram client bound for itself */
    if (clt->local_path_bound) {
        unlink(clt->ses.host_addr->un.sun_path);
        clt->local_path_bound = false;
    }
    return ret;
}

/*
 * The one place a lost connection or a failed connect is handled: tell the
 * user, close, and schedule the next attempt. on_close returning > 0 asks
 * for an immediate retry; that retry still goes through the event loop
 * (a zero delay timer) so a connect that keeps failing synchronously can
 * never recurse.
 */
/* tell the application that the connection is over. while it is told the
 * session refuses to send, quietly: a send that failed with an error would
 * bring us right back here for the same connection */
static int call_on_close(nw_clt *clt)
{
    int ret = 0;
    if (clt->type.on_close) {
        clt->in_on_close = true;
        clt->ses.closing = true;
        ret = clt->type.on_close(&clt->ses);
        clt->in_on_close = false;
    }
    return ret;
}

static void close_and_reconnect(nw_clt *clt)
{
    if (clt->in_on_close)
        return;
    int ret = call_on_close(clt);
    clt_close(clt);
    if (clt->close_requested) {
        /* on_close called nw_clt_close: stay closed */
        clt->close_requested = false;
        return;
    }
    reconnect_later(clt, ret > 0 ? 0 : clt->reconnect_timeout);
}

/*
 * a server given by name. connecting to it is: look the name up, then try
 * its addresses one by one. an address that fails is not worth telling
 * the application about while there are others left: this moves on to the
 * next one, through a zero delay timer like any retry, and returns true.
 * false means the attempt as a whole has failed.
 */
static bool try_next_addr(nw_clt *clt)
{
    if (clt->host == NULL || clt->resolve_again || clt->addr_next >= clt->addr_count)
        return false;
    clt_close(clt);
    reconnect_later(clt, 0);
    return true;
}

static int connect_to_peer(nw_clt *clt);

/* the answer of nw_dns */
static void on_resolved(int error, nw_addr_t *addrs, uint32_t count, void *privdata)
{
    nw_clt *clt = privdata;
    clt->dns_req = NULL;
    if (error == 0) {
        nw_addr_t *copy = realloc(clt->addrs, count * sizeof(nw_addr_t));
        if (copy == NULL) {
            /* out of memory: look it up again later */
            reconnect_later(clt, clt->reconnect_timeout);
            return;
        }
        memcpy(copy, addrs, count * sizeof(nw_addr_t));
        clt->addrs = copy;
        clt->addr_count = count;
        clt->addr_next = 0;
        clt->resolve_again = false;
    } else {
        /* a failed attempt like a connect that failed: say why, tell the
         * application it is over, try again later */
        char msg[NW_DNS_HOST_MAX + 128];
        snprintf(msg, sizeof(msg), "resolve %s: %s", clt->host, nw_dns_strerror(error));
        if (clt->type.on_error_msg)
            clt->type.on_error_msg(&clt->ses, msg);
        if (clt->ses.sock_type == SOCK_DGRAM) {
            /* nothing was open, there is nothing to close */
            reconnect_later(clt, clt->reconnect_timeout);
        } else {
            close_and_reconnect(clt);
        }
        return;
    }

    memcpy(&clt->ses.peer_addr, &clt->addrs[clt->addr_next++], sizeof(nw_addr_t));
    if (connect_to_peer(clt) < 0)
        reconnect_later(clt, clt->reconnect_timeout);
}

static void on_connect(nw_ses *ses, bool result)
{
    nw_clt *clt = (nw_clt *)ses;
    clt->on_connect_called = true;
    if (!result && try_next_addr(clt))
        return;
    if (result) {
        /* the connect timeout is not needed any more */
        nw_timer_stop(&clt->timer);
        clt->connected = true;
        /* when this connection is lost the name may point elsewhere */
        clt->resolve_again = true;
        set_socket_option(clt, clt->ses.sockfd);
        nw_sock_host_addr(ses->sockfd, ses->host_addr);
        if (clt->type.on_connect) {
            clt->type.on_connect(ses, result);
        }
    } else {
        if (clt->type.on_connect) {
            clt->type.on_connect(ses, result);
        }
        close_and_reconnect(clt);
    }
}

static void on_error(nw_ses *ses, const char *msg)
{
    nw_clt *clt = (nw_clt *)ses;
    if (clt->type.on_error_msg) {
        clt->type.on_error_msg(ses, msg);
    }
    if (ses->sock_type == SOCK_DGRAM)
        return;
    close_and_reconnect(clt);
}

static void on_close(nw_ses *ses)
{
    nw_clt *clt = (nw_clt *)ses;
    close_and_reconnect(clt);
}

int nw_clt_cfg_parse(const char *str, nw_clt_cfg *cfg)
{
    cfg->host[0] = '\0';
    cfg->port = 0;
    int ret = nw_sock_cfg_parse(str, &cfg->addr, &cfg->sock_type);
    /* -4: tcp or udp, with a port, but what stands before it is no ip */
    if (ret != -4)
        return ret;

    const char *host = strchr(str, '@') + 1;
    const char *colon = strrchr(host, ':');
    size_t len = colon - host;
    /* an ipv6 address that did not parse stays a bad ip */
    if (host[0] == '[' || memchr(host, ':', len) != NULL)
        return -4;
    if (len == 0 || len >= NW_DNS_HOST_MAX)
        return -6;
    bool only_digits_and_dots = true;
    for (size_t i = 0; i < len; ++i) {
        char c = host[i];
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        bool digit = c >= '0' && c <= '9';
        if (!letter && !digit && c != '-' && c != '_' && c != '.')
            return -6;
        if (!digit && c != '.')
            only_digits_and_dots = false;
    }
    /* 1.2.3 or 300.1.1.1: a mistyped ip, not a name */
    if (only_digits_and_dots)
        return -4;
    char *end = NULL;
    unsigned long port = strtoul(colon + 1, &end, 10);
    if (colon[1] < '0' || colon[1] > '9' || *end != '\0' || port > 65535)
        return -4;

    memcpy(cfg->host, host, len);
    cfg->host[len] = '\0';
    cfg->port = port;
    return 0;
}

nw_clt *nw_clt_create(nw_clt_cfg *cfg, nw_clt_type *type, void *privdata)
{
    nw_loop_init();

    if (cfg->max_pkg_size == 0)
        return NULL;
    if (type->decode_pkg == NULL)
        return NULL;
    if (type->on_recv_pkg == NULL)
        return NULL;

    nw_clt *clt = malloc(sizeof(nw_clt));
    if (clt == NULL)
        return NULL;
    memset(clt, 0, sizeof(nw_clt));
    clt->ses.sockfd = -1;
    clt->type = *type;
    clt->reconnect_timeout = cfg->reconnect_timeout  == 0 ? 1.0 : cfg->reconnect_timeout;
    if (cfg->buf_pool) {
        clt->custom_buf_pool = true;
        clt->buf_pool = cfg->buf_pool;
    } else {
        clt->custom_buf_pool = false;
        clt->buf_pool = nw_buf_pool_create(cfg->max_pkg_size);
        if (clt->buf_pool == NULL) {
            nw_clt_release(clt);
            return NULL;
        }
    }
    clt->read_mem = cfg->read_mem;
    clt->write_mem = cfg->write_mem;

    if (nw_ses_init(&clt->ses, nw_default_loop, clt->buf_pool, cfg->buf_limit, NW_SES_TYPE_CLIENT) < 0) {
        nw_clt_release(clt);
        return NULL;
    }
    nw_addr_t *host_addr = malloc(sizeof(nw_addr_t));
    if (host_addr == NULL) {
        nw_clt_release(clt);
        return NULL;
    }
    memset(host_addr, 0, sizeof(nw_addr_t));
    clt->ses.host_addr   = host_addr;
    if (cfg->host[0]) {
        /* by name: the address comes from nw_dns when the client starts */
        if (cfg->sock_type != SOCK_STREAM && cfg->sock_type != SOCK_DGRAM) {
            nw_clt_release(clt);
            return NULL;
        }
        clt->host = strdup(cfg->host);
        if (clt->host == NULL) {
            nw_clt_release(clt);
            return NULL;
        }
        clt->port = cfg->port;
        clt->resolve_again = true;
    } else {
        host_addr->family = cfg->addr.family;
        host_addr->addrlen = cfg->addr.addrlen;
        memcpy(&clt->ses.peer_addr, &cfg->addr, sizeof(nw_addr_t));
    }
    clt->ses.sockfd      = -1;
    clt->ses.sock_type   = cfg->sock_type;
    clt->ses.privdata    = privdata;

    clt->ses.decode_pkg  = type->decode_pkg;
    clt->ses.on_recv_pkg = type->on_recv_pkg;
    clt->ses.on_recv_fd  = type->on_recv_fd == NULL ? on_recv_fd : type->on_recv_fd;
    clt->ses.on_connect  = on_connect;
    clt->ses.on_error    = on_error;
    clt->ses.on_close    = on_close;
    clt->ses.on_drain    = type->on_drain;

    return clt;
}

int nw_clt_start(nw_clt *clt)
{
    /* starting an already started client restarts it, the old socket and
     * any pending reconnect or lookup are dropped */
    clt_close(clt);

    if (clt->host) {
        if (clt->resolve_again || clt->addr_next >= clt->addr_count) {
            /* connecting goes on in on_resolved */
            clt->on_connect_called = false;
            clt->dns_req = nw_dns_resolve(clt->host, clt->port, on_resolved, clt);
            return clt->dns_req ? 0 : -1;
        }
        memcpy(&clt->ses.peer_addr, &clt->addrs[clt->addr_next++], sizeof(nw_addr_t));
    }
    return connect_to_peer(clt);
}

/* open a socket and connect it to ses.peer_addr */
static int connect_to_peer(nw_clt *clt)
{
    int sockfd = create_socket(clt->ses.peer_addr.family, clt->ses.sock_type);
    if (sockfd < 0) {
        return -1;
    }
    clt->ses.sockfd = sockfd;
    /* the family can change from one address of a name to the next */
    clt->ses.host_addr->family = clt->ses.peer_addr.family;
    clt->ses.host_addr->addrlen = clt->ses.peer_addr.addrlen;
    if (clt->ses.peer_addr.family == AF_UNIX && clt->ses.sock_type == SOCK_DGRAM) {
        nw_addr_t *host_addr = clt->ses.host_addr;
        memset(host_addr, 0, sizeof(nw_addr_t));
        host_addr->family = AF_UNIX;
        host_addr->addrlen = sizeof(host_addr->un);
        host_addr->un.sun_family = AF_UNIX;
        generate_local_path(host_addr->un.sun_path, sizeof(host_addr->un.sun_path));
        if (nw_ses_bind(&clt->ses, host_addr) < 0) {
            int bind_errno = errno;
            clt_close(clt);
            errno = bind_errno;
            return -1;
        }
        clt->local_path_bound = true;
    }

    if (clt->ses.sock_type == SOCK_STREAM || clt->ses.sock_type == SOCK_SEQPACKET) {
        clt->connected = false;
        clt->on_connect_called = false;
        if (nw_ses_connect(&clt->ses, &clt->ses.peer_addr) < 0) {
            /* failed on the spot (unix path missing, backlog full ...):
             * same handling as a failure reported later by the loop */
            on_connect(&clt->ses, false);
            return 0;
        }
        if (!clt->on_connect_called) {
            watch_connect(clt);
        }
        return 0;
    } else {
        clt->connected = true;
        set_socket_option(clt, clt->ses.sockfd);
        nw_sock_host_addr(clt->ses.sockfd, clt->ses.host_addr);
        if (nw_ses_start(&clt->ses) < 0)
            return -1;
        if (clt->host) {
            /* a dgram client is usable at once, except when its server
             * had to be looked up first: say when it is */
            clt->resolve_again = true;
            if (clt->type.on_connect)
                clt->type.on_connect(&clt->ses, true);
        }
        return 0;
    }
}

int nw_clt_close(nw_clt *clt)
{
    if (clt->in_on_close) {
        /* from inside on_close: the close in progress finishes the job,
         * and does not reconnect */
        clt->close_requested = true;
        return 0;
    }
    call_on_close(clt);
    clt->close_requested = false;
    clt->resolve_again = true;
    return clt_close(clt);
}

void nw_clt_release(nw_clt *clt)
{
    /* stops the connect/reconnect timer: libev must not keep a pointer
     * into the memory freed below */
    clt_close(clt);
    nw_ses_release(&clt->ses);
    if (!clt->custom_buf_pool && clt->buf_pool) {
        nw_buf_pool_release(clt->buf_pool);
    }
    free(clt->ses.host_addr);
    free(clt->host);
    free(clt->addrs);
    free(clt);
}

bool nw_clt_connected(nw_clt *clt)
{
    return clt->connected;
}
