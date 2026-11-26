/*
 * Description: turn a host name into addresses, without blocking the loop
 *     History: yang@haipo.me, 2026/11/26, create
 */

# ifndef _NW_DNS_H_
# define _NW_DNS_H_

# include <stdint.h>

# include "nw_sock.h"

/*
 * nw_dns resolves a host name with the resolver of the system
 * (getaddrinfo: /etc/hosts, the name servers of /etc/resolv.conf, the
 * search domains, ipv4 and ipv6 in the order the system prefers), so a
 * name means here what it means for every other program on the machine.
 *
 * the system resolver blocks, sometimes for many seconds when a name
 * server does not answer. it is therefore run on a few threads of its own
 * (NW_DNS_THREADS per loop thread, started when first needed) and the
 * result comes back as a callback on the loop thread.
 *
 * nw_clt uses this for a server given by name, see nw_clt_cfg_parse: you
 * only need nw_dns directly for names of your own.
 *
 * a lookup that hangs can't be interrupted: it keeps one of the threads
 * busy until the system gives up, and releasing the loop of a thread
 * (nw_loop_release) waits for the lookups that are running.
 */

# define NW_DNS_THREADS     2
/* longest host name, with the terminating 0 */
# define NW_DNS_HOST_MAX    256
/* a name with more addresses than this gets the first ones */
# define NW_DNS_ADDR_MAX    16

typedef struct nw_dns_req nw_dns_req;

/*
 * the result of nw_dns_resolve, called once on the loop thread.
 * error is 0 and addrs holds count (at least 1) addresses, in the order
 * to try them, each with the port asked for; or error is not 0, see
 * nw_dns_strerror, and there are none.
 * addrs is only valid during the call, and the request is over: don't
 * cancel it any more.
 */
typedef void (*nw_dns_callback)(int error, nw_addr_t *addrs, uint32_t count, void *privdata);

/* resolve host, a name or a numeric ipv4 / ipv6 address. port is put into
 * the addresses. return the request, for nw_dns_cancel, or NULL if host is
 * empty or too long or memory or threads can't be had; then callback is
 * not called */
nw_dns_req *nw_dns_resolve(const char *host, uint16_t port, nw_dns_callback callback, void *privdata);

/* forget a request that has not been answered yet: its callback will not
 * be called. to be called on the thread that made the request */
void nw_dns_cancel(nw_dns_req *req);

/* what an error given to the callback means, for your log */
const char *nw_dns_strerror(int error);

# endif
