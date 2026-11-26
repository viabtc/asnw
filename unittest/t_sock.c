/* nw_sock_cfg_parse and address helpers */
# include "common.h"

int main(void)
{
    nw_addr addr;
    int type = 0;

    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1:1234", &addr, &type), 0);
    CHECK_EQ(type, SOCK_STREAM);
    CHECK_EQ(addr.family, AF_INET);
    CHECK_EQ(addr.addrlen, sizeof(struct sockaddr_in));
    CHECK_EQ(ntohs(addr.in.sin_port), 1234);
    CHECK(strcmp(nw_sock_human_addr(&addr), "127.0.0.1:1234") == 0);
    CHECK(strcmp(nw_sock_ip(&addr), "127.0.0.1") == 0);
    char human[NW_HUMAN_ADDR_SIZE], ip[NW_SOCK_IP_SIZE];
    CHECK(strcmp(nw_sock_human_addr_s(&addr, human), "127.0.0.1:1234") == 0);
    CHECK(strcmp(nw_sock_ip_s(&addr, ip), "127.0.0.1") == 0);

    CHECK_EQ(nw_sock_cfg_parse("UDP@10.0.0.1:53", &addr, &type), 0);
    CHECK_EQ(type, SOCK_DGRAM);
    CHECK_EQ(addr.family, AF_INET);

    CHECK_EQ(nw_sock_cfg_parse("stream@/tmp/a.sock", &addr, &type), 0);
    CHECK_EQ(type, SOCK_STREAM);
    CHECK_EQ(addr.family, AF_UNIX);
    CHECK(strcmp(addr.un.sun_path, "/tmp/a.sock") == 0);
    CHECK(strcmp(nw_sock_human_addr(&addr), "unix:/tmp/a.sock") == 0);
    CHECK(strcmp(nw_sock_ip(&addr), "") == 0);

    CHECK_EQ(nw_sock_cfg_parse("dgram@/tmp/a.sock", &addr, &type), 0);
    CHECK_EQ(type, SOCK_DGRAM);
    CHECK_EQ(nw_sock_cfg_parse("SeqPacket@/tmp/a.sock", &addr, &type), 0);
    CHECK_EQ(type, SOCK_SEQPACKET);

    CHECK_EQ(nw_sock_cfg_parse("tcp@[2001:db8::1]:443", &addr, &type), 0);
    CHECK_EQ(addr.family, AF_INET6);
    CHECK_EQ(addr.addrlen, sizeof(struct sockaddr_in6));
    CHECK(strcmp(nw_sock_human_addr(&addr), "[2001:db8::1]:443") == 0);
    CHECK(strcmp(nw_sock_ip(&addr), "2001:db8::1") == 0);

    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1:0", &addr, &type), 0);
    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1:65535", &addr, &type), 0);
    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1:65536", &addr, &type), -4);  /* port out of range */
    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1:http", &addr, &type), -4);   /* port not a number */
    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1:80x", &addr, &type), -4);
    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1:", &addr, &type), -4);
    CHECK_EQ(nw_sock_cfg_parse("tcp@[::1]", &addr, &type), -3);            /* no port */
    CHECK_EQ(nw_sock_cfg_parse("tcp@[::1:80", &addr, &type), -3);
    CHECK_EQ(nw_sock_cfg_parse("stream@", &addr, &type), -5);              /* empty path */

    CHECK_EQ(nw_sock_cfg_parse("127.0.0.1:1234", &addr, &type), -1);       /* no protocol */
    CHECK_EQ(nw_sock_cfg_parse("sctp@127.0.0.1:1234", &addr, &type), -2);  /* unknown protocol */
    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1", &addr, &type), -3);        /* no port */
    CHECK_EQ(nw_sock_cfg_parse("tcp@300.0.0.1:80", &addr, &type), -4);     /* bad ip */
    char longpath[300] = "stream@";
    memset(longpath + 7, 'a', 200);
    CHECK_EQ(nw_sock_cfg_parse(longpath, &addr, &type), -5);               /* path too long */

    /* socket option helpers on a real socket */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    CHECK_EQ(nw_sock_set_nonblock(fd), 0);
    CHECK(fcntl(fd, F_GETFL, 0) & O_NONBLOCK);
    CHECK_EQ(nw_sock_set_no_delay(fd), 0);
    CHECK_EQ(nw_sock_set_reuse_addr(fd), 0);
    CHECK_EQ(nw_sock_set_send_buf(fd, 65536), 0);
    int size = 0;
    CHECK_EQ(nw_sock_get_send_buf(fd, &size), 0);
    CHECK(size >= 65536);
    CHECK_EQ(nw_sock_errno(fd), 0);
    close(fd);

    return 0;
}
