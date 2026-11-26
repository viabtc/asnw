/*
 * Description: turn a host name into addresses, without blocking the loop
 *     History: yang@haipo.me, 2026/11/26, create
 */

# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <errno.h>
# include <netdb.h>

# include "nw_dns.h"
# include "nw_job.h"
# include "nw_evt.h"

struct nw_dns_req {
    char host[NW_DNS_HOST_MAX];
    uint16_t port;
    nw_dns_callback callback;
    void *privdata;
    /* set on the loop thread only */
    bool cancelled;
    /* the result, written by the resolver thread before it hands the
     * request back */
    int error;
    int sys_errno;
    uint32_t count;
    nw_addr_t addrs[NW_DNS_ADDR_MAX];
};

/* the resolver threads of the calling loop thread */
static __thread nw_job *resolver;

/* on a resolver thread */
static void on_job(nw_job_entry *entry, void *privdata)
{
    struct nw_dns_req *req = entry->request;
    char port[8];
    snprintf(port, sizeof(port), "%u", req->port);

    struct addrinfo hints;
    struct addrinfo *list = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    /* one entry per address, not one per socket type */
    hints.ai_socktype = SOCK_STREAM;
    req->error = getaddrinfo(req->host, port, &hints, &list);
    if (req->error != 0) {
        req->sys_errno = errno;
        return;
    }

    for (struct addrinfo *ai = list; ai && req->count < NW_DNS_ADDR_MAX; ai = ai->ai_next) {
        nw_addr_t *addr = &req->addrs[req->count];
        if (ai->ai_family == AF_INET && ai->ai_addrlen == sizeof(addr->in)) {
            memcpy(&addr->in, ai->ai_addr, sizeof(addr->in));
        } else if (ai->ai_family == AF_INET6 && ai->ai_addrlen == sizeof(addr->in6)) {
            memcpy(&addr->in6, ai->ai_addr, sizeof(addr->in6));
        } else {
            continue;
        }
        addr->family = ai->ai_family;
        addr->addrlen = ai->ai_addrlen;
        req->count++;
    }
    freeaddrinfo(list);
    if (req->count == 0)
        req->error = EAI_NONAME;
}

/* back on the loop thread */
static void on_finish(nw_job_entry *entry)
{
    struct nw_dns_req *req = entry->request;
    if (req->cancelled)
        return;
    if (req->error == EAI_SYSTEM)
        errno = req->sys_errno;
    req->callback(req->error, req->error ? NULL : req->addrs, req->count, req->privdata);
}

/* answered, cancelled or never run: the request ends here */
static void on_cleanup(nw_job_entry *entry)
{
    free(entry->request);
}

static void resolver_release(void)
{
    if (resolver) {
        nw_job_release(resolver);
        resolver = NULL;
    }
}

static int resolver_init(void)
{
    if (resolver)
        return 0;
    nw_job_type type;
    memset(&type, 0, sizeof(type));
    type.on_job = on_job;
    type.on_finish = on_finish;
    type.on_cleanup = on_cleanup;
    resolver = nw_job_create(&type, NW_DNS_THREADS);
    if (resolver == NULL)
        return -1;
    /* the threads go when the loop of this thread goes */
    if (nw_loop_at_release(resolver_release) < 0) {
        resolver_release();
        return -1;
    }
    return 0;
}

nw_dns_req *nw_dns_resolve(const char *host, uint16_t port, nw_dns_callback callback, void *privdata)
{
    size_t len = strlen(host);
    if (len == 0 || len >= NW_DNS_HOST_MAX)
        return NULL;
    if (resolver_init() < 0)
        return NULL;

    struct nw_dns_req *req = malloc(sizeof(struct nw_dns_req));
    if (req == NULL)
        return NULL;
    memset(req, 0, sizeof(struct nw_dns_req));
    memcpy(req->host, host, len);
    req->port = port;
    req->callback = callback;
    req->privdata = privdata;
    if (nw_job_add(resolver, 0, req) < 0) {
        free(req);
        return NULL;
    }
    return req;
}

void nw_dns_cancel(nw_dns_req *req)
{
    req->cancelled = true;
}

const char *nw_dns_strerror(int error)
{
    if (error == EAI_SYSTEM) {
        static __thread char text[80];
        snprintf(text, sizeof(text), "%m");
        return text;
    }
    return gai_strerror(error);
}
