/*
 * Shared helpers for asnw unit tests. Every test is one executable:
 * exit 0 = pass, anything else (assert, crash, sanitizer report) = fail.
 */
# ifndef _UT_COMMON_H_
# define _UT_COMMON_H_

# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <unistd.h>
# include <errno.h>
# include <fcntl.h>
# include <signal.h>

# include "nw_svr.h"
# include "nw_clt.h"
# include "nw_timer.h"
# include "nw_state.h"
# include "nw_job.h"

# define UT_UNUSED __attribute__((unused))

# define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

# define CHECK_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "%s:%d: CHECK_EQ failed: %s = %lld, %s = %lld\n", \
                __FILE__, __LINE__, #a, _a, #b, _b); \
        exit(1); \
    } \
} while (0)

/* '\n' terminated messages */
UT_UNUSED static int ut_line_decode(nw_ses *ses, void *data, size_t max)
{
    char *s = data;
    for (size_t i = 0; i < max; ++i) {
        if (s[i] == '\n')
            return i + 1;
    }
    return 0;
}

static nw_timer ut_stop_timer;

static void ut_on_stop(nw_timer *timer, void *privdata)
{
    nw_loop_break();
}

/* run the loop for at most `sec` seconds; nw_loop_break() ends it earlier */
UT_UNUSED static void ut_run_for(double sec)
{
    nw_timer_set(&ut_stop_timer, sec, false, ut_on_stop, NULL);
    nw_timer_start(&ut_stop_timer);
    nw_loop_run();
    nw_timer_stop(&ut_stop_timer);
}

static char ut_dir[64];

static void ut_rm_tmpdir(void)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", ut_dir);
    if (system(cmd) != 0) {
        /* best effort */
    }
}

/* a private directory for unix socket paths, removed at exit */
UT_UNUSED static const char *ut_tmpdir(void)
{
    if (ut_dir[0] == 0) {
        strcpy(ut_dir, "/tmp/asnw-ut-XXXXXX");
        CHECK(mkdtemp(ut_dir) != NULL);
        atexit(ut_rm_tmpdir);
    }
    return ut_dir;
}

/* "stream@/tmp/asnw-ut-xxxx/name" */
UT_UNUSED static void ut_unix_cfg(char *out, size_t size, const char *proto, const char *name)
{
    snprintf(out, size, "%s@%s/%s", proto, ut_tmpdir(), name);
}

UT_UNUSED static nw_svr *ut_svr(const char *cfg_str, nw_svr_type *type, uint32_t max_pkg_size, void *privdata)
{
    static nw_svr_bind bind;
    CHECK(nw_sock_cfg_parse(cfg_str, &bind.addr, &bind.sock_type) == 0);
    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.bind_arr = &bind;
    cfg.max_pkg_size = max_pkg_size;
    nw_svr *svr = nw_svr_create(&cfg, type, privdata);
    CHECK(svr != NULL);
    CHECK(nw_svr_start(svr) == 0);
    return svr;
}

UT_UNUSED static nw_clt *ut_clt(const char *cfg_str, nw_clt_type *type, uint32_t max_pkg_size, double reconnect, void *privdata)
{
    nw_clt_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(nw_clt_cfg_parse(cfg_str, &cfg) == 0);
    cfg.max_pkg_size = max_pkg_size;
    cfg.reconnect_timeout = reconnect;
    nw_clt *clt = nw_clt_create(&cfg, type, privdata);
    CHECK(clt != NULL);
    return clt;
}

/* port actually bound by the i-th listener (bind to port 0, then ask) */
UT_UNUSED static int ut_svr_port(nw_svr *svr, int i)
{
    nw_addr addr;
    CHECK(nw_sock_host_addr(svr->bind_list[i].sockfd, &addr) == 0);
    return ntohs(addr.in.sin_port);
}

/* a local tcp/udp port that nobody listens on */
UT_UNUSED static int ut_free_port(int sock_type)
{
    int fd = socket(AF_INET, sock_type, 0);
    CHECK(fd >= 0);
    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(in);
    CHECK(bind(fd, (struct sockaddr *)&in, len) == 0);
    CHECK(getsockname(fd, (struct sockaddr *)&in, &len) == 0);
    close(fd);
    return ntohs(in.sin_port);
}

# endif
