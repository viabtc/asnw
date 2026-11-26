/*
 * Description: server, listens on one or more addresses
 *     History: yang@haipo.me, 2016/03/20, create
 */

# include <math.h>
# include <stdlib.h>
# include <stdio.h>
# include <string.h>
# include <unistd.h>

# include "nw_svr.h"

/* how long a connection that is shutting down is given to hang up */
# define NW_SVR_LINGER_TIME     2.0
/* the timeout timer normally runs once a second, faster if a timeout is
 * shorter than that asks for, but not faster than this */
# define NW_SVR_TIMER_INTERVAL  1.0
# define NW_SVR_TIMER_MIN       0.05

static void on_error(nw_ses *ses, const char *msg);
static void on_timer(nw_timer *timer, void *privdata);

/* how often the connections have to be looked at for a timeout of
 * `period` seconds to be noticed in time */
static double interval_for(double period)
{
    double interval = NW_SVR_TIMER_INTERVAL;
    if (period < 2 * interval)
        interval = period / 2;
    if (interval < NW_SVR_TIMER_MIN)
        interval = NW_SVR_TIMER_MIN;
    return interval;
}

static void set_timer(nw_svr *svr, double interval)
{
    nw_timer_stop(&svr->timer);
    nw_timer_set(&svr->timer, interval, true, on_timer, svr);
    nw_timer_start(&svr->timer);
    svr->timer_interval = interval;
}

/*
 * walk the connections: close the ones done shutting down, report the
 * ones whose idle timeout or deadline has passed.
 *
 * on the way, note the shortest timeout that is still in force, and let
 * that decide when to look again. so the timer runs as often as the
 * connections of the moment need - not as often as some connection once
 * needed - and stops when none of them has anything to wait for.
 */
static void on_timer(nw_timer *timer, void *privdata)
{
    nw_svr *svr = privdata;
    double now = ev_now(nw_default_loop);
    double shortest = INFINITY;

    /* timeouts set by the callbacks below are taken into account at the
     * end, see need_timer */
    svr->in_sweep = true;
    svr->sweep_shortest = INFINITY;

    nw_svr_iterator iter;
    nw_ses *ses;
    nw_svr_iter_init(&iter, svr);
    while ((ses = nw_svr_next(&iter)) != NULL) {
        if (ses->closing) {
            if (now >= ses->close_deadline) {
                nw_svr_close_clt(svr, ses);
            } else if (NW_SVR_LINGER_TIME < shortest) {
                shortest = NW_SVR_LINGER_TIME;
            }
            continue;
        }
        bool expired = false;
        if (ses->deadline > 0) {
            if (now >= ses->deadline) {
                ses->deadline = 0;
                expired = true;
            } else if (ses->deadline_span < shortest) {
                shortest = ses->deadline_span;
            }
        }
        if (ses->idle_timeout > 0) {
            /* silent since the last data, or since the timeout last fired */
            double since = ses->active_time > ses->idle_mark ? ses->active_time : ses->idle_mark;
            if (now - since >= ses->idle_timeout) {
                ses->idle_mark = now;
                expired = true;
            }
            /* if it stays open it times out again one period later */
            if (ses->idle_timeout < shortest)
                shortest = ses->idle_timeout;
        }
        if (!expired)
            continue;
        if (svr->type.on_timeout) {
            svr->type.on_timeout(ses);
        } else {
            on_error(ses, "timeout");
        }
    }

    svr->in_sweep = false;
    if (svr->sweep_shortest < shortest)
        shortest = svr->sweep_shortest;
    if (shortest == INFINITY) {
        /* nothing to watch: the next connection that needs it starts it again */
        nw_timer_stop(&svr->timer);
        svr->timer_interval = 0;
        return;
    }
    double interval = interval_for(shortest);
    if (interval != svr->timer_interval)
        set_timer(svr, interval);
}

/* a connection got a timeout of `period` seconds: make sure the timer
 * runs, and often enough for it */
static void need_timer(nw_svr *svr, double period)
{
    if (svr->in_sweep) {
        /* the walk in progress sets the timer when it is done */
        if (period < svr->sweep_shortest)
            svr->sweep_shortest = period;
        return;
    }
    double interval = interval_for(period);
    if (nw_timer_active(&svr->timer) && svr->timer_interval <= interval)
        return;
    set_timer(svr, interval);
}

static int create_socket(int family, int sock_type)
{
    int sockfd = socket(family, sock_type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sockfd < 0)
        return -1;
    if (nw_sock_set_reuse_addr(sockfd) < 0) {
        close(sockfd);
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
static void set_socket_option(nw_svr *svr, int sockfd)
{
    if (svr->read_mem > 0) {
        nw_sock_set_recv_buf(sockfd, svr->read_mem);
    }
    if (svr->write_mem > 0) {
        nw_sock_set_send_buf(sockfd, svr->write_mem);
    }
}

static void on_error(nw_ses *ses, const char *msg)
{
    nw_svr *svr = (nw_svr *)ses->svr;
    if (svr->type.on_error_msg) {
        svr->type.on_error_msg(ses, msg);
    }
    if (ses->ses_type == NW_SES_TYPE_COMMON) {
        nw_svr_close_clt(svr, ses);
    }
}

static void on_close(nw_ses *ses)
{
    nw_svr *svr = (nw_svr *)ses->svr;
    if (ses->ses_type == NW_SES_TYPE_COMMON) {
        nw_svr_close_clt(svr, ses);
    }
}

static void on_recv_fd(nw_ses *ses, int fd)
{
    close(fd);
}

/* the single place that frees a server; also used on a half built one */
static void nw_svr_free(nw_svr *svr)
{
    if (svr->svr_list) {
        for (uint32_t i = 0; i < svr->svr_count; ++i) {
            nw_ses *ses = &svr->svr_list[i];
            if (ses->write_buf != NULL) {
                nw_ses_release(ses);
            }
            free(ses->host_addr);
        }
        free(svr->svr_list);
    }
    if (svr->clt_dict)
        nw_dict_release(svr->clt_dict);
    /* sessions hand their bufs back to the pool, so the pool goes last */
    if (svr->clt_cache)
        nw_cache_release(svr->clt_cache);
    if (svr->buf_pool)
        nw_buf_pool_release(svr->buf_pool);
    free(svr);
}

static int nw_svr_add_clt(nw_ses *ses, int sockfd, nw_addr *peer_addr)
{
    nw_svr *svr = (nw_svr *)ses->svr;
    set_socket_option(svr, sockfd);

    void *privdata = NULL;
    if (svr->type.on_privdata_alloc) {
        privdata = svr->type.on_privdata_alloc(svr);
        if (privdata == NULL) {
            return -1;
        }
    }

    nw_ses *clt = nw_cache_alloc(svr->clt_cache);
    if (clt == NULL) {
        if (privdata) {
            svr->type.on_privdata_free(svr, privdata);
        }
        return -1;
    }
    memset(clt, 0, sizeof(nw_ses));
    if (nw_ses_init(clt, nw_default_loop, svr->buf_pool, svr->buf_limit, NW_SES_TYPE_COMMON) < 0) {
        nw_cache_free(svr->clt_cache, clt);
        if (privdata) {
            svr->type.on_privdata_free(svr, privdata);
        }
        return -1;
    }
    memcpy(&clt->peer_addr, peer_addr, sizeof(nw_addr));
    clt->host_addr   = ses->host_addr;
    clt->sockfd      = sockfd;
    clt->sock_type   = ses->sock_type;
    clt->privdata    = privdata;
    clt->svr         = svr;

    clt->id = svr->id_start++;
    if (clt->id == 0)
        clt->id = svr->id_start++;
    if (nw_dict_add(svr->clt_dict, clt->id, clt) < 0) {
        /* nothing else knows this session yet, undo it. sockfd is closed
         * by the caller, as for every failure of this function */
        clt->sockfd = -1;
        nw_ses_release(clt);
        nw_cache_free(svr->clt_cache, clt);
        if (privdata) {
            svr->type.on_privdata_free(svr, privdata);
        }
        return -1;
    }

    clt->decode_pkg  = svr->type.decode_pkg;
    clt->on_recv_pkg = svr->type.on_recv_pkg;
    clt->on_recv_fd  = svr->type.on_recv_fd == NULL ? on_recv_fd : svr->type.on_recv_fd;
    clt->on_error    = on_error;
    clt->on_close    = on_close;
    clt->on_drain    = svr->type.on_drain;

    svr->clt_count++;
    clt->active_time = ev_now(nw_default_loop);
    clt->idle_timeout = svr->idle_timeout;
    if (clt->idle_timeout > 0)
        need_timer(svr, clt->idle_timeout);
    nw_ses_start(clt);

    if (svr->type.on_new_connection) {
        svr->type.on_new_connection(clt);
    }

    return 0;
}

static int on_accept(nw_ses *ses, int sockfd, nw_addr *peer_addr)
{
    nw_svr *svr = (nw_svr *)ses->svr;
    if (svr->type.on_accept) {
        return svr->type.on_accept(ses, sockfd, peer_addr);
    }
    return nw_svr_add_clt(ses, sockfd, peer_addr);
}

int nw_svr_add_clt_fd(nw_svr *svr, int fd)
{
    nw_addr peer_addr;
    if (nw_sock_peer_addr(fd, &peer_addr) < 0) {
        return -1;
    }
    /* it comes from somewhere else: make it what an accepted one is */
    if (nw_sock_set_nonblock(fd) < 0 || nw_sock_set_cloexec(fd) < 0) {
        return -1;
    }
    nw_ses *ses = NULL;
    for (uint32_t i = 0; i < svr->svr_count; ++i) {
        if (peer_addr.family == svr->svr_list[i].host_addr->family) {
            ses = &svr->svr_list[i];
            break;
        }
    }
    if (ses == NULL)
        return -1;
    return nw_svr_add_clt(ses, fd, &peer_addr);
}

int nw_svr_add_accepted_fd(nw_svr *svr, uint32_t bind_index, int fd, nw_addr *peer_addr)
{
    if (bind_index >= svr->svr_count)
        return -1;
    nw_ses *ses = &svr->svr_list[bind_index];
    if (ses->sock_type == SOCK_DGRAM)
        return -1;
    return nw_svr_add_clt(ses, fd, peer_addr);
}

nw_svr *nw_svr_create(nw_svr_cfg *cfg, nw_svr_type *type, void *privdata)
{
    nw_loop_init();

    if (cfg->bind_count == 0)
        return NULL;
    if (cfg->max_pkg_size == 0)
        return NULL;
    if (type->decode_pkg == NULL)
        return NULL;
    if (type->on_recv_pkg == NULL)
        return NULL;
    if (type->on_privdata_alloc && !type->on_privdata_free)
        return NULL;

    nw_svr *svr = malloc(sizeof(nw_svr));
    if (svr == NULL)
        return NULL;
    memset(svr, 0, sizeof(nw_svr));
    svr->type = *type;
    svr->svr_count = cfg->bind_count;
    svr->svr_list = calloc(svr->svr_count, sizeof(nw_ses));
    if (svr->svr_list == NULL) {
        nw_svr_free(svr);
        return NULL;
    }
    svr->buf_pool = nw_buf_pool_create(cfg->max_pkg_size);
    if (svr->buf_pool == NULL) {
        nw_svr_free(svr);
        return NULL;
    }
    svr->clt_cache = nw_cache_create(sizeof(nw_ses));
    if (svr->clt_cache == NULL) {
        nw_svr_free(svr);
        return NULL;
    }
    svr->clt_dict = nw_dict_create(0);
    if (svr->clt_dict == NULL) {
        nw_svr_free(svr);
        return NULL;
    }
    svr->idle_timeout = cfg->idle_timeout > 0 ? cfg->idle_timeout : 0;
    svr->buf_limit = cfg->buf_limit;
    svr->read_mem = cfg->read_mem;
    svr->write_mem = cfg->write_mem;
    svr->privdata = privdata;
    for (uint32_t i = 0; i < svr->svr_count; ++i) {
        nw_ses *ses = &svr->svr_list[i];
        int sockfd = create_socket(cfg->bind_arr[i].addr.family, cfg->bind_arr[i].sock_type);
        if (sockfd < 0) {
            nw_svr_free(svr);
            return NULL;
        }
        nw_addr *host_addr = malloc(sizeof(nw_addr));
        if (host_addr == NULL) {
            close(sockfd);
            nw_svr_free(svr);
            return NULL;
        }
        memcpy(host_addr, &cfg->bind_arr[i].addr, sizeof(nw_addr));
        if (nw_ses_init(ses, nw_default_loop, svr->buf_pool, svr->buf_limit, NW_SES_TYPE_SERVER) < 0) {
            close(sockfd);
            free(host_addr);
            nw_svr_free(svr);
            return NULL;
        }
        ses->sockfd      = sockfd;
        ses->sock_type   = cfg->bind_arr[i].sock_type;
        ses->host_addr   = host_addr;
        ses->svr         = svr;

        ses->on_accept   = on_accept;
        ses->decode_pkg  = type->decode_pkg;
        ses->on_recv_pkg = type->on_recv_pkg;
        ses->on_recv_fd  = type->on_recv_fd == NULL ? on_recv_fd : type->on_recv_fd;
        ses->on_error    = on_error;
        ses->on_close    = on_close;

        if (cfg->bind_arr[i].sock_type == SOCK_DGRAM) {
            ses->peer_addr.family = host_addr->family;
            ses->peer_addr.addrlen = host_addr->addrlen;
            set_socket_option(svr, sockfd);
        }
    }

    return svr;
}

int nw_svr_start(nw_svr *svr)
{
    for (uint32_t i = 0; i < svr->svr_count; ++i) {
        nw_ses *ses = &svr->svr_list[i];
        if (nw_ses_bind(ses, ses->host_addr) < 0) {
            return -1;
        }
        if (nw_ses_start(ses) < 0) {
            return -1;
        }
    }

    return 0;
}

int nw_svr_stop(nw_svr *svr)
{
    for (uint32_t i = 0; i < svr->svr_count; ++i) {
        if (nw_ses_stop(&svr->svr_list[i]) < 0) {
            return -1;
        }
    }
    return 0;
}

void nw_svr_release(nw_svr *svr)
{
    nw_svr_stop(svr);
    nw_timer_stop(&svr->timer);
    nw_svr_iterator iter;
    nw_ses *curr;
    nw_svr_iter_init(&iter, svr);
    while ((curr = nw_svr_next(&iter)) != NULL) {
        nw_svr_close_clt(svr, curr);
    }
    nw_svr_free(svr);
}

void nw_svr_close_clt(nw_svr *svr, nw_ses *ses)
{
    if (ses->id == 0)
        return;
    if (ses->ses_type != NW_SES_TYPE_COMMON)
        return;

    /*
     * take the connection out of the server BEFORE telling the application.
     * whatever on_connection_close does then - send on this connection,
     * close it again, close others whose own callbacks look for this one -
     * cannot get back in here for it: it is not found by id any more, a
     * second close stops at the line below, and sending fails quietly
     * instead of reporting an error that would close it once more.
     */
    if (nw_dict_del(svr->clt_dict, ses->id) < 0)
        return;
    svr->clt_count--;
    ses->closing = true;
    nw_ses_stop(ses);

    if (svr->type.on_connection_close) {
        svr->type.on_connection_close(ses);
    }
    if (ses->privdata) {
        svr->type.on_privdata_free(svr, ses->privdata);
    }
    nw_ses_release(ses);
    nw_cache_free(svr->clt_cache, ses);
}

nw_ses *nw_svr_get_clt(nw_svr *svr, uint64_t id)
{
    return nw_dict_get(svr->clt_dict, id);
}

int nw_svr_send(nw_svr *svr, uint64_t id, const void *data, size_t size)
{
    nw_ses *ses = nw_svr_get_clt(svr, id);
    if (ses == NULL)
        return -1;
    return nw_ses_send(ses, data, size);
}

void nw_svr_iter_init(nw_svr_iterator *iter, nw_svr *svr)
{
    nw_dict_iter_init(&iter->iter, svr->clt_dict);
}

nw_ses *nw_svr_next(nw_svr_iterator *iter)
{
    /* connections closed since the iterator got here (by a callback of the
     * loop that is running) are skipped: a closed session has lost its id */
    nw_dict_entry *entry;
    while ((entry = nw_dict_next(&iter->iter)) != NULL) {
        nw_ses *ses = entry->val;
        if (ses->id == entry->key)
            return ses;
    }
    return NULL;
}

static bool is_clt(nw_ses *ses)
{
    return ses->ses_type == NW_SES_TYPE_COMMON && ses->id != 0;
}

void nw_svr_shutdown_clt(nw_svr *svr, nw_ses *ses)
{
    if (!is_clt(ses) || ses->closing)
        return;
    ses->close_deadline = ev_now(nw_default_loop) + NW_SVR_LINGER_TIME;
    nw_ses_shutdown(ses);
    need_timer(svr, NW_SVR_LINGER_TIME);
}

void nw_svr_set_clt_idle_timeout(nw_svr *svr, nw_ses *ses, double timeout)
{
    if (!is_clt(ses))
        return;
    ses->idle_timeout = timeout > 0 ? timeout : 0;
    ses->idle_mark = ev_now(nw_default_loop);
    if (ses->idle_timeout > 0)
        need_timer(svr, ses->idle_timeout);
}

void nw_svr_set_clt_deadline(nw_svr *svr, nw_ses *ses, double timeout)
{
    if (!is_clt(ses))
        return;
    if (timeout <= 0) {
        ses->deadline = 0;
        return;
    }
    ses->deadline = ev_now(nw_default_loop) + timeout;
    ses->deadline_span = timeout;
    need_timer(svr, timeout);
}
