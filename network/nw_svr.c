/*
 * Description: 
 *     History: yang@haipo.me, 2016/03/20, create
 */

# include <stdlib.h>
# include <stdio.h>
# include <unistd.h>

# include "nw_svr.h"

static int create_socket(int family, int sock_type)
{
    int sockfd = socket(family, sock_type, 0);
    if (sockfd < 0)
        return -1;
    if (nw_sock_set_nonblock(sockfd) < 0) {
        close(sockfd);
        return -1;
    }
    if (nw_sock_set_reuse_addr(sockfd) < 0) {
        close(sockfd);
        return -1;
    }
    if (sock_type == SOCK_STREAM && (family == AF_INET || family == AF_INET6)) {
        if (nw_sock_set_defer_accept(sockfd) < 0) {
            close(sockfd);
            return -1;
        }
        if (nw_sock_set_no_delay(sockfd) < 0) {
            close(sockfd);
            return -1;
        }
    }

    return sockfd;
}

static int set_socket_option(nw_svr *svr, int sockfd)
{
    if (svr->read_mem > 0) {
        if (nw_sock_set_recv_buf(sockfd, svr->read_mem) < 0) {
            close(sockfd);
            return -1;
        }
    }
    if (svr->write_mem > 0) {
        if (nw_sock_set_send_buf(sockfd, svr->write_mem) < 0) {
            close(sockfd);
            return -1;
        }
    }

    return 0;
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

static void nw_svr_free(nw_svr *svr)
{
    if (svr->buf_pool)
        nw_buf_pool_release(svr->buf_pool);
    if (svr->clt_cache)
        nw_cache_release(svr->clt_cache);
    if (svr->svr_list) {
        for (uint32_t i = 0; i < svr->svr_count; ++i) {
            if (svr->svr_list[i].write_buf != NULL) {
                nw_ses_release(&svr->svr_list[i]);
                free(svr->svr_list[i].host_addr);
            }
        }
        free(svr->svr_list);
    }
    free(svr);
}

static int nw_svr_add_clt(nw_ses *ses, int sockfd, nw_addr_t *peer_addr)
{
    nw_svr *svr = (nw_svr *)ses->svr;
    set_socket_option(svr, sockfd);
    if (nw_sock_set_nonblock(sockfd) < 0) {
        return -1;
    }

    void *privdata = NULL;
    if (svr->type.on_privdata_alloc) {
        privdata = svr->type.on_privdata_alloc();
        if (privdata == NULL) {
            return -1;
        }
    }

    nw_ses *clt = nw_cache_alloc(svr->clt_cache);
    if (clt == NULL) {
        return -1;
    }
    memset(clt, sizeof(nw_ses), 0);
    if (nw_ses_init(clt, nw_default_loop, svr->buf_pool, NW_SES_TYPE_COMMON) < 0) {
        nw_cache_free(svr->clt_cache, clt);
        if (privdata) {
            svr->type.on_privdata_free(privdata);
        }
        return -1;
    }
    memcpy(&clt->peer_addr, peer_addr, sizeof(nw_addr_t));
    clt->host_addr   = ses->host_addr;
    clt->sockfd      = sockfd;
    clt->sock_type   = ses->sock_type;
    clt->magic       = svr->id_start++;
    clt->privdata    = privdata;
    clt->svr         = svr;

    clt->decode_pkg  = svr->type.decode_pkg;
    clt->on_recv_pkg = svr->type.on_recv_pkg;
    clt->on_recv_fd  = svr->type.on_recv_fd == NULL ? on_recv_fd : svr->type.on_recv_fd;
    clt->on_error    = on_error;
    clt->on_close    = on_close;

    clt->next = svr->clt_list_head;
    if (clt->next) {
        clt->next->prev = clt;
    }
    svr->clt_list_head = clt;
    svr->clt_count++;
    if (svr->type.on_new_connection) {
        svr->type.on_new_connection(clt);
    }

    nw_ses_start(clt);

    return 0;
}

static int on_accept(nw_ses *ses, int sockfd, nw_addr_t *peer_addr)
{
    return nw_svr_add_clt(ses, sockfd, peer_addr);
}

int nw_svr_add_clt_fd(nw_svr *svr, int fd)
{
    nw_addr_t peer_addr;
    if (nw_sock_peer_addr(fd, &peer_addr) < 0) {
        return -1;
    }
    nw_ses *ses = NULL;
    for (uint32_t i = 0; i < svr->svr_count; ++i) {
        if (peer_addr.family == svr->svr_list[i].host_addr->family) {
            ses = &svr->svr_list[i];
        }
    }
    if (ses == NULL)
        return -1;
    return nw_svr_add_clt(ses, fd, &peer_addr);
}

nw_svr *nw_svr_create(nw_svr_cfg *cfg, nw_svr_type *type)
{
    nw_loop_init();

    if (type->on_recv_pkg == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < cfg->bind_count; ++i) {
        if (cfg->bind_arr[i].sock_type == SOCK_STREAM && type->decode_pkg == NULL) {
            return NULL;
        }
    }
    if (type->on_privdata_alloc && !type->on_privdata_free) {
        return NULL;
    }

    nw_svr *svr = malloc(sizeof(nw_svr));
    if (svr == NULL)
        return NULL;
    memset(svr, sizeof(nw_svr), 0);
    svr->type = *type;
    svr->svr_count = cfg->bind_count;
    svr->svr_list = malloc(sizeof(nw_ses) * svr->svr_count);
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
    svr->read_mem = cfg->read_mem;
    svr->write_mem = cfg->write_mem;
    memset(svr->svr_list, sizeof(nw_ses) * svr->svr_count, 0);
    for (uint32_t i = 0; i < svr->svr_count; ++i) {
        nw_ses *ses = &svr->svr_list[i];
        int sockfd = create_socket(cfg->bind_arr[i].addr.family, cfg->bind_arr[i].sock_type);
        if (sockfd < 0) {
            nw_svr_free(svr);
            return NULL;
        }
        nw_addr_t *host_addr = malloc(sizeof(nw_addr_t));
        if (host_addr == NULL) {
            nw_svr_free(svr);
            return NULL;
        }
        memcpy(host_addr, &cfg->bind_arr[i].addr, sizeof(nw_addr_t));
        if (nw_ses_init(ses, nw_default_loop, svr->buf_pool, NW_SES_TYPE_SERVER) < 0) {
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

        if (nw_ses_bind(ses, host_addr) < 0) {
            nw_svr_free(svr);
            return NULL;
        }
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
        if (nw_ses_start(&svr->svr_list[i]) < 0) {
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
    nw_ses *curr = svr->clt_list_head;
    while (curr) {
        nw_ses *next = curr->next;
        if (curr->privdata) {
            svr->type.on_privdata_free(curr->privdata);
        }
        nw_ses_release(curr);
        nw_cache_free(svr->clt_cache, curr);
        curr = next;
    }
    nw_cache_release(svr->clt_cache);
    nw_svr_free(svr);
}

void nw_svr_close_clt(nw_svr *svr, nw_ses *ses)
{
    if (svr->type.on_connection_close) {
        svr->type.on_connection_close(ses);
    }
    if (ses->prev) {
        ses->prev->next = ses->next;
    }
    if (ses->next) {
        ses->next->prev = ses->prev;
    }
    if (ses->privdata) {
        svr->type.on_privdata_free(ses->privdata);
    }
    nw_ses_release(ses);
    nw_cache_free(svr->clt_cache, ses);
    svr->clt_count--;
}

