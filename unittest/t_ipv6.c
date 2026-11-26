/*
 * Regression (fixed): nw_sock_cfg_parse() could not parse any IPv6 address,
 * it split host and port at the FIRST ':'. Both the bracket form and the
 * bare form (port after the last colon) are accepted now. An IPv6 echo
 * round trip is part of t_echo.c.
 */
# include "common.h"

static void expect(const char *cfg, const char *ip, int port)
{
    nw_addr addr;
    int type;
    CHECK_EQ(nw_sock_cfg_parse(cfg, &addr, &type), 0);
    CHECK_EQ(addr.family, AF_INET6);
    CHECK_EQ(ntohs(addr.in6.sin6_port), port);
    CHECK(strcmp(nw_sock_ip(&addr), ip) == 0);
}

int main(void)
{
    expect("tcp@[::1]:8080", "::1", 8080);
    expect("tcp@::1:8080", "::1", 8080);
    expect("udp@[::]:53", "::", 53);
    expect("tcp@[fe80::1234:5678]:1", "fe80::1234:5678", 1);
    expect("tcp@[::ffff:10.0.0.1]:80", "::ffff:10.0.0.1", 80);
    return 0;
}
