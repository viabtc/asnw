/*
 * Description: socket related
 *     History: yang@haipo.me, 2016/03/16, create
 */

# ifndef _NW_SOCK_H_
# define _NW_SOCK_H_

# include <sys/types.h>
# include <sys/resource.h>
# include <sys/stat.h>
# include <sys/socket.h>
# include <sys/un.h>
# include <arpa/inet.h>
# include <netinet/in.h>
# include <netinet/tcp.h>

typedef struct nw_addr_t {
    unsigned int family;
    unsigned int addrlen;
    union {
        struct sockaddr_in  in;
        struct sockaddr_in6 in6;
        struct sockaddr_un  un;
    };
} nw_addr_t;

# define NW_SOCKADDR(addr) ((struct sockaddr *)(&(addr)->in))
# define NW_HUMAN_ADDR_SIZE 128

char *nw_sock_human_addr(nw_addr_t *addr);
char *nw_sock_human_addr_s(nw_addr_t *addr, char *dest);

int nw_sock_cfg_parse(const char *cfg, nw_addr_t *addr, int *sock_type);

int nw_sock_peer_addr(int sockfd, nw_addr_t *addr);
int nw_sock_host_addr(int sockfd, nw_addr_t *addr);
int nw_sock_errno(int sockfd);

int nw_sock_get_send_buf(int sockfd, int *buf_size);
int nw_sock_get_recv_buf(int sockfd, int *buf_size);
int nw_sock_set_send_buf(int sockfd, int buf_size);
int nw_sock_set_recv_buf(int sockfd, int buf_size);

int nw_sock_set_nonblock(int sockfd);
int nw_sock_set_no_delay(int sockfd);
int nw_sock_set_reuse_addr(int sockfd);

# endif

