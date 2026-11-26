/*
 * Description: network session, one socket with its buffers and callbacks
 *     History: yang@haipo.me, 2016/03/19, create
 */

# ifndef _GNU_SOURCE
# define _GNU_SOURCE     /* accept4 */
# endif
# include <stdio.h>
# include <string.h>
# include <errno.h>
# include <fcntl.h>
# include <unistd.h>
# include <pthread.h>

# include "nw_ses.h"

static void libev_on_read_write_evt(struct ev_loop *loop, ev_io *watcher, int events);
static void libev_on_accept_evt(struct ev_loop *loop, ev_io *watcher, int events);
static void libev_on_connect_evt(struct ev_loop *loop, ev_io *watcher, int events);

/* spare fd, see shed_connection(). one for the process, shared by the
 * threads */
static int idle_fd = -1;
static pthread_mutex_t idle_lock = PTHREAD_MUTEX_INITIALIZER;

static void watch_stop(nw_ses *ses)
{
    if (ev_is_active(&ses->ev)) {
        ev_io_stop(ses->loop, &ses->ev);
    }
}

/* a session whose reading is paused (nw_ses_pause_read) watches for
 * nothing, or for writable only */
static void watch_read(nw_ses *ses)
{
    if (ev_is_active(&ses->ev)) {
        ev_io_stop(ses->loop, &ses->ev);
    }
    ev_io_init(&ses->ev, libev_on_read_write_evt, ses->sockfd, EV_READ);
    if (!ses->read_paused)
        ev_io_start(ses->loop, &ses->ev);
}

static void watch_read_write(nw_ses *ses)
{
    if (ev_is_active(&ses->ev)) {
        ev_io_stop(ses->loop, &ses->ev);
    }
    ev_io_init(&ses->ev, libev_on_read_write_evt, ses->sockfd, ses->read_paused ? EV_WRITE : EV_READ | EV_WRITE);
    ev_io_start(ses->loop, &ses->ev);
}

/* watch for what the session needs right now */
static void watch_update(nw_ses *ses)
{
    if (ses->write_buf->count > 0) {
        watch_read_write(ses);
    } else {
        watch_read(ses);
    }
}

static void watch_accept(nw_ses *ses)
{
    ev_io_init(&ses->ev, libev_on_accept_evt, ses->sockfd, EV_READ);
    ev_io_start(ses->loop, &ses->ev);
}

static void watch_connect(nw_ses *ses)
{
    ev_io_init(&ses->ev, libev_on_connect_evt, ses->sockfd, EV_WRITE);
    ev_io_start(ses->loop, &ses->ev);
}

/* returns the number of bytes written; if less than size, errno tells why */
static size_t nw_write_stream(nw_ses *ses, const void *data, size_t size)
{
    size_t spos = 0;
    while (spos < size) {
        int ret = send(ses->sockfd, data + spos, size - spos, MSG_NOSIGNAL);
        if (ret > 0) {
            spos += ret;
        } else if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        } else {
            break;
        }
    }

    return spos;
}

static int nw_write_packet(nw_ses *ses, const void *data, size_t size)
{
    while (true) {
        struct msghdr msg;
        struct iovec io;

        memset(&msg, 0, sizeof(msg));
        io.iov_base = (void *)data;
        io.iov_len = size;
        msg.msg_iov = &io;
        msg.msg_iovlen = 1;

        int ret = sendmsg(ses->sockfd, &msg, MSG_EOR | MSG_NOSIGNAL);
        if (ret < 0 && errno == EINTR) {
            continue;
        } else {
            return ret;
        }
    }
}

/* is this datagram from the server a client session is configured for? */
static bool dgram_from_peer(nw_addr *peer, nw_addr *from)
{
    if (peer->family == AF_INET) {
        /* replies to a broadcast, multicast or wildcard target come from
         * the responders' own addresses */
        uint32_t ip = ntohl(peer->in.sin_addr.s_addr);
        if (ip == INADDR_ANY || ip == INADDR_BROADCAST || IN_MULTICAST(ip))
            return true;
        return from->family == AF_INET && from->in.sin_port == peer->in.sin_port && \
            from->in.sin_addr.s_addr == peer->in.sin_addr.s_addr;
    } else if (peer->family == AF_INET6) {
        if (IN6_IS_ADDR_UNSPECIFIED(&peer->in6.sin6_addr) || IN6_IS_ADDR_MULTICAST(&peer->in6.sin6_addr))
            return true;
        return from->family == AF_INET6 && from->in6.sin6_port == peer->in6.sin6_port && \
            memcmp(&from->in6.sin6_addr, &peer->in6.sin6_addr, sizeof(struct in6_addr)) == 0;
    }
    /* unix: only local processes can reach the socket and the same server
     * may be known under different path spellings, don't filter */
    return true;
}

/*
 * The process (or system) is out of fds. The pending connection stays in the
 * backlog and keeps the listen fd readable, so just returning would spin the
 * loop at 100% cpu. Give up the spare fd, accept the connection and close it
 * right away: the peer sees a clean close, the backlog drains.
 *
 * return 1: a connection was shed, 0: the backlog is empty (accept reports
 * EMFILE before it even looks at the backlog), -1: could not shed.
 */
static int shed_connection(nw_ses *ses)
{
    int ret = -1;
    pthread_mutex_lock(&idle_lock);
    if (idle_fd >= 0) {
        close(idle_fd);
        int sockfd = accept4(ses->sockfd, NULL, NULL, SOCK_CLOEXEC);
        if (sockfd >= 0) {
            close(sockfd);
            ret = 1;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            ret = 0;
        }
        idle_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    }
    pthread_mutex_unlock(&idle_lock);
    return ret;
}

/*
 * hand the complete messages in the read buffer of a stream session to
 * the application. return false if on_can_read has to stop there: the
 * session was closed, or its reading was paused by the callback.
 */
static bool stream_decode(nw_ses *ses)
{
    size_t size = 0;
    while ((size = nw_buf_size(ses->read_buf)) > 0) {
        int ret = ses->decode_pkg(ses, ses->read_buf->data + ses->read_buf->rpos, size);
        if (ret < 0) {
            char errmsg[100];
            snprintf(errmsg, sizeof(errmsg), "decode msg error: %d", ret);
            ses->on_error(ses, errmsg);
            return false;
        } else if (ret > 0) {
            ses->on_recv_pkg(ses, ses->read_buf->data + ses->read_buf->rpos, ret);
            if (!ses->read_buf)
                return false;
            if (ses->closing) {
                /* the callback shut the session down: the rest
                 * of what was read goes the same way */
                ses->read_buf->rpos = ses->read_buf->wpos = 0;
                break;
            }
            ses->read_buf->rpos += ret;
            if (ses->read_paused) {
                /* the callback paused reading: what is left waits here
                 * for nw_ses_resume_read */
                if (nw_buf_size(ses->read_buf) == 0) {
                    nw_buf_free(ses->pool, ses->read_buf);
                    ses->read_buf = NULL;
                } else {
                    nw_buf_shift(ses->read_buf);
                    ses->resume_decode = true;
                }
                return false;
            }
        } else {
            nw_buf_shift(ses->read_buf);
            if (ses->read_buf->wpos == ses->read_buf->size) {
                ses->on_error(ses, "decode msg error");
                return false;
            }
            break;
        }
    }

    nw_buf_shift(ses->read_buf);
    return true;
}

static void on_can_read(nw_ses *ses)
{
    if (ses->sockfd < 0 || ses->read_paused)
        return;
    if (ses->read_buf == NULL) {
        ses->read_buf = nw_buf_alloc(ses->pool);
        if (ses->read_buf == NULL) {
            ses->on_error(ses, "no recv buf");
            return;
        }
    }

    switch (ses->sock_type) {
    case SOCK_STREAM:
        {
            if (ses->resume_decode) {
                /* messages held back while reading was paused */
                ses->resume_decode = false;
                if (!stream_decode(ses))
                    return;
            }
            while (true) {
                size_t want = nw_buf_avail(ses->read_buf);
                int ret = read(ses->sockfd, ses->read_buf->data + ses->read_buf->wpos, want);
                if (ret < 0) {
                    if (errno == EINTR) {
                        continue;
                    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    } else {
                        char errmsg[100];
                        snprintf(errmsg, sizeof(errmsg), "read error: %m");
                        ses->on_error(ses, errmsg);
                        return;
                    }
                } else if (ret == 0) {
                    ses->on_close(ses);
                    return;
                } else {
                    ses->read_buf->wpos += ret;
                    ses->active_time = ev_now(ses->loop);
                }
                if (ses->closing) {
                    /* shutting down: read on, so that the peer's close is
                     * seen, but nobody wants the data any more */
                    ses->read_buf->rpos = ses->read_buf->wpos = 0;
                } else if (!stream_decode(ses)) {
                    return;
                }
                /* less than asked for: the socket is empty, another read
                 * would only say so. what comes next, the end of the
                 * stream included, is announced by the event loop */
                if ((size_t)ret < want)
                    break;
            }
            if (nw_buf_size(ses->read_buf) == 0) {
                nw_buf_free(ses->pool, ses->read_buf);
                ses->read_buf = NULL;
            }
        }
        break;
    case SOCK_DGRAM:
        {
            while (true) {
                /* paused by the callback: the rest stays in the kernel */
                if (ses->read_paused)
                    break;
                /* addrlen is value-result: it must be reset before every call,
                 * otherwise a short source address truncates all later ones */
                nw_addr from;
                memset(&from, 0, sizeof(from));
                from.addrlen = sizeof(from.un);
                int ret = recvfrom(ses->sockfd, ses->read_buf->data, ses->read_buf->size, 0, \
                        NW_SOCKADDR(&from), &from.addrlen);
                if (ret < 0) {
                    if (errno == EINTR) {
                        continue;
                    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    } else {
                        char errmsg[100];
                        snprintf(errmsg, sizeof(errmsg), "recvfrom error: %m");
                        ses->on_error(ses, errmsg);
                        return;
                    }
                }
                from.family = from.addrlen ? NW_SOCKADDR(&from)->sa_family : ses->peer_addr.family;
                if (ses->ses_type == NW_SES_TYPE_CLIENT) {
                    /* a client keeps talking to the configured server: its
                     * peer_addr is never replaced by a source address, and
                     * inet datagrams from anybody else are dropped */
                    if (!dgram_from_peer(&ses->peer_addr, &from))
                        continue;
                } else {
                    /* a server replies to the sender of the current datagram */
                    memcpy(&ses->peer_addr, &from, sizeof(nw_addr));
                }
                int pkg_size = ret;
                ret = ses->decode_pkg(ses, ses->read_buf->data, pkg_size);
                if (ret < 0) {
                    char errmsg[100];
                    snprintf(errmsg, sizeof(errmsg), "decode msg error: %d", ret);
                    ses->on_error(ses, errmsg);
                    if (!ses->read_buf)
                        return;
                    continue;
                }
                if (ret == 0) {
                    /* not a full message: a datagram can't be completed later, drop it */
                    continue;
                }
                ses->on_recv_pkg(ses, ses->read_buf->data, ret);
                if (!ses->read_buf)
                    return;
            }
            nw_buf_free(ses->pool, ses->read_buf);
            ses->read_buf = NULL;
        }
        break;
    case SOCK_SEQPACKET:
        {
            while (true) {
                if (ses->read_paused)
                    break;
                struct msghdr msg;
                struct iovec io;
                char control[CMSG_SPACE(sizeof(int))];

                memset(&msg, 0, sizeof(msg));
                io.iov_base = ses->read_buf->data;
                io.iov_len = ses->read_buf->size;
                msg.msg_iov = &io;
                msg.msg_iovlen = 1;
                msg.msg_control = control;
                msg.msg_controllen = sizeof(control);

                /* a file descriptor that comes along is close-on-exec */
                int ret = recvmsg(ses->sockfd, &msg, MSG_CMSG_CLOEXEC);
                if (ret < 0) {
                    if (errno == EINTR) {
                       continue;
                    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    } else {
                        char errmsg[100];
                        snprintf(errmsg, sizeof(errmsg), "recvmsg error: %m");
                        ses->on_error(ses, errmsg);
                        return;
                    }
                } else if (ret == 0) {
                    ses->on_close(ses);
                    return;
                }
                ses->active_time = ev_now(ses->loop);

                struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
                if (cmsg != NULL) {
                    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                        int fd;
                        memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
                        if (ses->closing) {
                            close(fd);
                            continue;
                        }
                        ses->on_recv_fd(ses, fd);
                        /* the callback may have closed this session */
                        if (!ses->read_buf)
                            return;
                    }
                } else {
                    if (ses->closing)
                        continue;
                    int pkg_size = ret;
                    ret = ses->decode_pkg(ses, ses->read_buf->data, pkg_size);
                    if (ret < 0) {
                        char errmsg[100];
                        snprintf(errmsg, sizeof(errmsg), "decode msg error: %d", ret);
                        ses->on_error(ses, errmsg);
                        return;
                    }
                    if (ret == 0) {
                        /* not a full message: a packet can't be completed later, drop it */
                        continue;
                    }
                    ses->on_recv_pkg(ses, ses->read_buf->data, ret);
                    if (!ses->read_buf)
                        return;
                }
            }
            nw_buf_free(ses->pool, ses->read_buf);
            ses->read_buf = NULL;
        }
        break;
    }
}

static void on_can_write(nw_ses *ses)
{
    if (ses->sockfd < 0)
        return;

    while (ses->write_buf->count > 0) {
        nw_buf *buf = ses->write_buf->head;
        size_t size = nw_buf_size(buf);
        bool done;
        if (ses->sock_type == SOCK_STREAM) {
            /* returns the bytes written so far, never negative */
            size_t nwrite = nw_write_stream(ses, buf->data + buf->rpos, size);
            buf->rpos += nwrite;
            ses->write_pending -= nwrite;
            done = (nwrite == size);
        } else {
            /* a packet is sent whole or not at all, -1 on failure */
            done = (nw_write_packet(ses, buf->data + buf->rpos, size) >= 0);
            if (done)
                ses->write_pending -= size;
        }
        if (!done) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* keep it queued, wait for the next writable event */
                break;
            } else {
                char errmsg[100];
                snprintf(errmsg, sizeof(errmsg), "write error: %m");
                ses->on_error(ses, errmsg);
                return;
            }
        }
        nw_buf_list_shift(ses->write_buf);
    }

    if (ses->write_buf->count == 0) {
        watch_read(ses);
        /* everything is out: a session that is shutting down can tell the
         * peer that nothing more will come */
        if (ses->closing) {
            if (!ses->write_shut) {
                shutdown(ses->sockfd, SHUT_WR);
                ses->write_shut = true;
            }
        } else if (ses->on_drain) {
            /* the last thing done here: the callback may close the session */
            ses->on_drain(ses);
        }
    }
}

static void on_can_accept(nw_ses *ses)
{
    if (ses->sockfd < 0)
        return;

    while (true) {
        nw_addr peer_addr;
        memset(&peer_addr, 0, sizeof(peer_addr));
        peer_addr.family = ses->local_addr->family;
        peer_addr.addrlen = ses->local_addr->addrlen;
        /* non-blocking and close-on-exec from the start */
        int sockfd = accept4(ses->sockfd, NW_SOCKADDR(&peer_addr), &peer_addr.addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (sockfd < 0) {
            if (errno == EINTR || errno == ECONNABORTED || errno == EPROTO) {
                /* ECONNABORTED: that connection is gone, the next one may be fine */
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else if (errno == EMFILE || errno == ENFILE) {
                int accept_errno = errno;
                int shed = shed_connection(ses);
                if (shed == 0)
                    break;
                char errmsg[100];
                errno = accept_errno;
                snprintf(errmsg, sizeof(errmsg), "accept error: %m%s", shed > 0 ? ", connection rejected" : "");
                ses->on_error(ses, errmsg);
                if (shed < 0 || ses->sockfd < 0)
                    return;
            } else {
                char errmsg[100];
                snprintf(errmsg, sizeof(errmsg), "accept error: %m");
                ses->on_error(ses, errmsg);
                return;
            }
        } else {
            int ret = ses->on_accept(ses, sockfd, &peer_addr);
            if (ret < 0) {
                close(sockfd);
            }
        }
    }
}

static void on_can_connect(nw_ses *ses)
{
    if (ses->sockfd < 0)
        return;
    errno = nw_sock_errno(ses->sockfd);
    if (errno != 0) {
        ses->on_connect(ses, false);
        return;
    }
    watch_read(ses);
    ses->on_connect(ses, true);
}

static void libev_on_read_write_evt(struct ev_loop *loop, ev_io *watcher, int events)
{
    nw_ses *ses = (nw_ses *)watcher;
    if (events & EV_READ)
        on_can_read(ses);
    /* a read callback may have closed the session, or closed and restarted
     * it (nw_clt reconnect): then the watcher is no longer this one */
    if (!ev_is_active(&ses->ev) || ev_cb(&ses->ev) != libev_on_read_write_evt)
        return;
    if (events & EV_WRITE)
        on_can_write(ses);
}

static void libev_on_accept_evt(struct ev_loop *loop, ev_io *watcher, int events)
{
    nw_ses *ses = (nw_ses *)watcher;
    if (events & EV_READ)
        on_can_accept(ses);
}

static void libev_on_connect_evt(struct ev_loop *loop, ev_io *watcher, int events)
{
    nw_ses *ses = (nw_ses *)watcher;
    watch_stop(ses);
    if (events & EV_WRITE)
        on_can_connect(ses);
}

int nw_ses_bind(nw_ses *ses, nw_addr *addr)
{
    if (addr->family == AF_UNIX) {
        /* remove a stale socket left by a previous run, but never anything else */
        struct stat st;
        if (lstat(addr->un.sun_path, &st) == 0 && S_ISSOCK(st.st_mode)) {
            unlink(addr->un.sun_path);
        }
    }
    int ret = bind(ses->sockfd, NW_SOCKADDR(addr), addr->addrlen);
    if (ret < 0)
        return ret;
    if (addr->family == AF_UNIX) {
        return nw_sock_set_mode(addr, 0777);
    }
    return 0;
}

int nw_ses_listen(nw_ses *ses, int backlog)
{
    int ret = listen(ses->sockfd, backlog);
    if (ret < 0)
        return -1;
    pthread_mutex_lock(&idle_lock);
    if (idle_fd < 0)
        idle_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    pthread_mutex_unlock(&idle_lock);
    watch_accept(ses);
    return 0;
}

int nw_ses_connect(nw_ses *ses, nw_addr *addr)
{
    int ret = connect(ses->sockfd, NW_SOCKADDR(addr), addr->addrlen);
    if (ret == 0) {
        watch_read(ses);
        ses->on_connect(ses, true);
        return 0;
    }
    if (errno == EINPROGRESS) {
        watch_connect(ses);
        return 0;
    }
    /* immediate failure: errno is set, the caller handles it (on_connect is
     * NOT called here, so the failure is processed exactly once) */
    return -1;
}

int nw_ses_start(nw_ses *ses)
{
    if (ses->ses_type == NW_SES_TYPE_SERVER && (ses->sock_type == SOCK_STREAM || ses->sock_type == SOCK_SEQPACKET)) {
        return nw_ses_listen(ses, SOMAXCONN);
    } else {
        watch_read(ses);
    }
    return 0;
}

int nw_ses_stop(nw_ses *ses)
{
    watch_stop(ses);
    return 0;
}

int nw_ses_send(nw_ses *ses, const void *data, size_t size)
{
    if (ses->sockfd < 0 || ses->closing) {
        return -1;
    }

    if (ses->write_buf->count > 0) {
        size_t nwrite;
        if (ses->sock_type == SOCK_STREAM) {
            nwrite = nw_buf_list_write(ses->write_buf, data, size);
        } else {
            nwrite = nw_buf_list_append(ses->write_buf, data, size);
        }
        if (nwrite != size) {
            ses->on_error(ses, "no send buf");
            return -1;
        }
        ses->write_pending += size;
    } else {
        switch (ses->sock_type) {
        case SOCK_STREAM:
            {
                size_t nwrite = nw_write_stream(ses, data, size);
                if (nwrite < size) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        if (nw_buf_list_write(ses->write_buf, data + nwrite, size - nwrite) != (size - nwrite)) {
                            ses->on_error(ses, "no send buf");
                            return -1;
                        }
                        ses->write_pending += size - nwrite;
                        watch_read_write(ses);
                    } else {
                        char errmsg[100];
                        snprintf(errmsg, sizeof(errmsg), "write error: %m");
                        ses->on_error(ses, errmsg);
                        return -1;
                    }
                }
            }
            break;
        case SOCK_DGRAM:
            {
                int ret = sendto(ses->sockfd, data, size, MSG_NOSIGNAL, NW_SOCKADDR(&ses->peer_addr), ses->peer_addr.addrlen);
                if (ret < 0) {
                    char errmsg[100];
                    snprintf(errmsg, sizeof(errmsg), "sendto error: %m");
                    ses->on_error(ses, errmsg);
                    return -1;
                }
            }
            break;
        case SOCK_SEQPACKET:
            {
                int ret = nw_write_packet(ses, data, size);
                if (ret < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        if (nw_buf_list_append(ses->write_buf, data, size) != size) {
                            ses->on_error(ses, "no send buf");
                            return -1;
                        }
                        ses->write_pending += size;
                        watch_read_write(ses);
                    } else {
                        char errmsg[100];
                        snprintf(errmsg, sizeof(errmsg), "sendmsg error: %m");
                        ses->on_error(ses, errmsg);
                        return -1;
                    }
                }
            }
            break;
        default:
            break;
        }
    }

    return 0;
}

size_t nw_ses_pending(nw_ses *ses)
{
    return ses->write_pending;
}

/* is this a session that reads and writes data right now: not a listening
 * one, not one that is still connecting, not a closed one */
static bool is_flowing(nw_ses *ses)
{
    return ses->sockfd >= 0 && ev_cb(&ses->ev) == libev_on_read_write_evt;
}

void nw_ses_pause_read(nw_ses *ses)
{
    if (!is_flowing(ses) || ses->read_paused || ses->closing)
        return;
    ses->read_paused = true;
    watch_update(ses);
}

void nw_ses_resume_read(nw_ses *ses)
{
    if (!is_flowing(ses) || !ses->read_paused)
        return;
    ses->read_paused = false;
    watch_update(ses);
    /* look at the socket, and at what was held back, on the next turn of
     * the loop: the caller may be in the middle of something */
    ev_feed_event(ses->loop, &ses->ev, EV_READ);
}

int nw_ses_send_fd(nw_ses *ses, int fd)
{
    if (ses->sockfd < 0 || ses->sock_type != SOCK_SEQPACKET) {
        return -1;
    }

    struct msghdr msg;
    struct iovec io;
    char control[CMSG_SPACE(sizeof(int))];

    memset(&msg, 0, sizeof(msg));
    memset(control, 0, sizeof(control));
    io.iov_base = &fd;
    io.iov_len = sizeof(fd);
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *((int *)CMSG_DATA(cmsg)) = fd;

    return sendmsg(ses->sockfd, &msg, MSG_EOR | MSG_NOSIGNAL);
}

int nw_ses_init(nw_ses *ses, struct ev_loop *loop, nw_buf_pool *pool, uint32_t buf_limit, int ses_type)
{
    memset(ses, 0, sizeof(nw_ses));
    ses->sockfd = -1;
    ses->loop = loop;
    ses->ses_type = ses_type;
    ses->pool = pool;
    ses->write_buf = nw_buf_list_create(pool, buf_limit);
    if (ses->write_buf == NULL) {
        return -1;
    }

    return 0;
}

void nw_ses_shutdown(nw_ses *ses)
{
    if (ses->sockfd < 0 || ses->closing)
        return;
    ses->closing = true;
    if (ses->read_paused) {
        /* read again, to see the peer hang up; the data is thrown away */
        ses->read_paused = false;
        ses->resume_decode = false;
        watch_update(ses);
    }
    if (ses->write_buf->count == 0) {
        shutdown(ses->sockfd, SHUT_WR);
        ses->write_shut = true;
    }
}

int nw_ses_close(nw_ses *ses)
{
    watch_stop(ses);
    ses->id = 0;
    ses->closing = false;
    ses->write_shut = false;
    ses->deadline = 0;
    ses->idle_mark = 0;
    ses->write_pending = 0;
    ses->read_paused = false;
    ses->resume_decode = false;
    if (ses->sockfd >= 0) {
        close(ses->sockfd);
        ses->sockfd = -1;
    }
    if (ses->read_buf) {
        nw_buf_free(ses->pool, ses->read_buf);
        ses->read_buf = NULL;
    }
    if (ses->write_buf) {
        while (ses->write_buf->count) {
            nw_buf_list_shift(ses->write_buf);
        }
    }

    return 0;
}

int nw_ses_release(nw_ses *ses)
{
    nw_ses_close(ses);
    if (ses->write_buf) {
        nw_buf_list_release(ses->write_buf);
        ses->write_buf = NULL;
    }

    return 0;
}
