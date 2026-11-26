/*
 * Flow control: nw_ses_pending, on_drain, nw_ses_pause_read and
 * nw_ses_resume_read, and what they are for: passing data from a fast
 * sender to a slow receiver without piling it up.
 */
# include <sys/socket.h>
# include <sys/un.h>
# include "common.h"

# define CHUNK (64 * 1024)

static nw_svr *svr;
static nw_addr addr;

/* what the server does with a connection is set by the test */
static int recv_count, drain_count, close_count;
static char recv_log[4096];
static bool pause_each;                 /* pause after every message */
static nw_ses *last_ses;
static uint64_t relay_from, relay_to;   /* pass everything from one connection to another */
static size_t relay_mark, relay_max_pending, relayed;
static int relay_pauses;
static bool relay_overflow;

static int raw_decode(nw_ses *ses, void *data, size_t max)
{
    /* relay: whatever is there. otherwise lines */
    if (relay_to && ses->id == relay_from)
        return max;
    return ut_line_decode(ses, data, max);
}

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    last_ses = ses;
    if (relay_to && ses->id == relay_from) {
        nw_ses *down = nw_svr_get_clt(svr, relay_to);
        if (down == NULL)
            return;
        if (nw_ses_send(down, data, size) < 0) {
            /* its queue was full: it is closed and gone */
            relay_overflow = true;
            return;
        }
        relayed += size;
        size_t pending = nw_ses_pending(down);
        if (pending > relay_max_pending)
            relay_max_pending = pending;
        if (pending > relay_mark) {
            nw_ses_pause_read(ses);
            relay_pauses++;
        }
        return;
    }
    recv_count++;
    size_t used = strlen(recv_log);
    if (used + size < sizeof(recv_log)) {
        memcpy(recv_log + used, data, size);
        recv_log[used + size] = 0;
    }
    if (pause_each)
        nw_ses_pause_read(ses);
}

static void on_drain(nw_ses *ses)
{
    drain_count++;
    CHECK_EQ(nw_ses_pending(ses), 0);
    if (relay_to && ses->id == relay_to) {
        nw_ses *up = nw_svr_get_clt(svr, relay_from);
        if (up)
            nw_ses_resume_read(up);
    }
}

static void on_connection_close(nw_ses *ses)
{
    close_count++;
}

static void on_resume_timer(nw_timer *timer, void *privdata)
{
    if (last_ses && last_ses->id != 0)
        nw_ses_resume_read(last_ses);
}

static int connect_tcp(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    CHECK_EQ(connect(fd, NW_SOCKADDR(&addr), sizeof(addr.in)), 0);
    return fd;
}

/* the connection the server made for the client that connected last */
static nw_ses *newest(void)
{
    nw_svr_iterator iter;
    nw_ses *ses, *found = NULL;
    ut_run_for(0.05);
    nw_svr_iter_init(&iter, svr);
    while ((ses = nw_svr_next(&iter)) != NULL) {
        if (found == NULL || ses->id > found->id)
            found = ses;
    }
    CHECK(found != NULL);
    return found;
}

/* read what is there, without blocking. return the bytes read */
static size_t drain_fd(int fd, uint32_t *sum)
{
    static char buf[CHUNK];
    size_t total = 0;
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n <= 0)
            break;
        for (ssize_t i = 0; i < n; ++i)
            *sum = *sum * 31 + (unsigned char)buf[i];
        total += n;
    }
    return total;
}

/* nw_clt side */
static int clt_drains, clt_connected;
static void clt_on_connect(nw_ses *ses, bool result) { clt_connected += result; }
static void clt_on_recv_pkg(nw_ses *ses, void *data, size_t size) { }
static void clt_on_drain(nw_ses *ses) { clt_drains++; }

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    static nw_svr_bind svr_bind;
    CHECK_EQ(nw_sock_cfg_parse("tcp@127.0.0.1:0", &svr_bind.addr, &svr_bind.sock_type), 0);
    nw_svr_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_count = 1;
    cfg.bind_arr = &svr_bind;
    cfg.max_pkg_size = CHUNK;
    cfg.buf_limit = 16;                 /* 1M may wait for a connection, then it is closed */
    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = raw_decode;
    type.on_recv_pkg = on_recv_pkg;
    type.on_connection_close = on_connection_close;
    type.on_drain = on_drain;
    svr = nw_svr_create(&cfg, &type, NULL);
    CHECK(svr != NULL);
    CHECK_EQ(nw_svr_start(svr), 0);
    CHECK_EQ(nw_sock_host_addr(svr->svr_list[0].sockfd, &addr), 0);

    static char chunk[CHUNK];
    uint32_t sent_sum = 0, got_sum = 0;

    /*
     * pending and on_drain: send to a client that does not read
     */
    int fd = connect_tcp();
    nw_ses *ses = newest();
    CHECK_EQ(nw_ses_pending(ses), 0);
    CHECK_EQ(nw_ses_send(ses, "small\n", 6), 0);        /* goes out at once: nothing pending, no drain */
    CHECK_EQ(nw_ses_pending(ses), 0);
    size_t sent = 6;
    for (int i = 0; i < 6; ++i)
        sent_sum = sent_sum * 31 + (unsigned char)"small\n"[i];
    /* until the kernel has had enough and 512K wait in the session */
    while (nw_ses_pending(ses) < 512 * 1024) {
        for (int i = 0; i < CHUNK; ++i) {
            chunk[i] = (char)(sent + i * 7);
            sent_sum = sent_sum * 31 + (unsigned char)chunk[i];
        }
        CHECK_EQ(nw_ses_send(ses, chunk, CHUNK), 0);
        sent += CHUNK;
        CHECK(sent < 64 * 1024 * 1024);
    }
    size_t pending = nw_ses_pending(ses);
    CHECK(pending >= 512 * 1024 && pending < sent);
    /* it counts bytes, the list counts bufs */
    CHECK(ses->write_buf->count >= pending / CHUNK && ses->write_buf->count <= pending / CHUNK + 1);
    ut_run_for(0.05);
    CHECK_EQ(drain_count, 0);
    CHECK_EQ(nw_ses_pending(ses), pending);             /* nobody reads, nothing moves */

    size_t got = 0;
    for (int i = 0; i < 2000 && got < sent; ++i) {
        got += drain_fd(fd, &got_sum);
        ut_run_for(0.002);
        CHECK(nw_ses_pending(ses) <= pending);
    }
    CHECK_EQ(got, sent);
    CHECK_EQ(got_sum, sent_sum);
    CHECK_EQ(drain_count, 1);                           /* once, when the last byte was out */
    CHECK_EQ(nw_ses_pending(ses), 0);
    close(fd);
    ut_run_for(0.05);
    CHECK_EQ(close_count, 1);

    /*
     * pause and resume: messages are held back, in order, including those
     * that were read from the socket already
     */
    fd = connect_tcp();
    ses = newest();
    pause_each = true;
    CHECK_EQ(send(fd, "1\n2\n3\n4\n5\n", 10, 0), 10);   /* one read, five messages */
    ut_run_for(0.05);
    CHECK_EQ(recv_count, 1);                            /* paused inside the first */
    CHECK(strcmp(recv_log, "1\n") == 0);
    CHECK_EQ(send(fd, "6\n7\n", 4, 0), 4);
    ut_run_for(0.1);
    CHECK_EQ(recv_count, 1);                            /* new data does not wake it up */
    nw_ses_pause_read(ses);                             /* paused already: nothing */

    /* one message per resume, with no new data arriving */
    for (int i = 2; i <= 7; ++i) {
        nw_ses_resume_read(ses);
        CHECK_EQ(recv_count, i - 1);                    /* not from inside the call */
        ut_run_for(0.02);
        CHECK_EQ(recv_count, i);
    }
    CHECK(strcmp(recv_log, "1\n2\n3\n4\n5\n6\n7\n") == 0);

    /* resumed by a timer, half a message at a time arriving meanwhile */
    nw_timer timer;
    nw_timer_set(&timer, 0.01, true, on_resume_timer, NULL);
    nw_timer_start(&timer);
    CHECK_EQ(send(fd, "8\n9\n1", 5, 0), 5);
    ut_run_for(0.1);
    CHECK_EQ(send(fd, "0\n11\n", 5, 0), 5);
    ut_run_for(0.2);
    nw_timer_stop(&timer);
    CHECK_EQ(recv_count, 11);
    CHECK(strcmp(recv_log, "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n") == 0);
    pause_each = false;

    /* a paused connection does not see its peer leave; resumed, it does */
    nw_ses_pause_read(ses);
    close(fd);
    ut_run_for(0.1);
    CHECK_EQ(close_count, 1);
    nw_ses_resume_read(ses);
    ut_run_for(0.05);
    CHECK_EQ(close_count, 2);

    /* shutting a paused connection down reads again, to see the hang up */
    fd = connect_tcp();
    ses = newest();
    nw_ses_pause_read(ses);
    CHECK_EQ(nw_ses_send(ses, "bye\n", 4), 0);
    nw_svr_shutdown_clt(svr, ses);
    CHECK(!ses->read_paused);
    nw_ses_pause_read(ses);                             /* too late for that */
    CHECK(!ses->read_paused);
    char small[16];
    CHECK_EQ(recv(fd, small, sizeof(small), 0), 4);
    CHECK_EQ(recv(fd, small, sizeof(small), 0), 0);     /* the server said it is done */
    close(fd);
    ut_run_for(0.1);
    CHECK_EQ(close_count, 3);

    /*
     * what it is for: a relay, from a fast sender to a slow receiver
     * through a connection that may queue 1M at most.
     *
     * first without flow control (the mark is never reached): the queue of
     * the receiver overflows and it is dropped, which is what the library
     * does when nobody says otherwise.
     */
    int up = connect_tcp();
    relay_from = newest()->id;
    int down = connect_tcp();
    relay_to = newest()->id;
    relay_mark = (size_t)-1;
    CHECK_EQ(fcntl(up, F_SETFL, O_NONBLOCK), 0);
    for (int turn = 0; turn < 20000 && !relay_overflow; ++turn) {
        while (send(up, chunk, CHUNK, MSG_NOSIGNAL) > 0)
            ;
        ut_run_for(0.001);
    }
    CHECK(relay_overflow);
    CHECK(nw_svr_get_clt(svr, relay_to) == NULL);
    CHECK_EQ(relay_pauses, 0);
    CHECK_EQ(close_count, 4);
    close(up);
    close(down);
    ut_run_for(0.1);
    CHECK_EQ(close_count, 5);

    /*
     * now with it: 8M, the sender is made to wait, never more than the
     * mark plus one read is queued, and nobody is dropped.
     */
    up = connect_tcp();
    relay_from = newest()->id;
    down = connect_tcp();
    relay_to = newest()->id;
    CHECK(relay_from != relay_to);
    relay_mark = 256 * 1024;
    relay_max_pending = relayed = 0;
    relay_overflow = false;
    drain_count = 0;
    CHECK_EQ(fcntl(up, F_SETFL, O_NONBLOCK), 0);

    size_t total = 8 * 1024 * 1024, written = 0, received = 0;
    sent_sum = got_sum = 0;
    for (int turn = 0; turn < 200000 && received < total; ++turn) {
        /* the sender pushes as hard as it can */
        while (written < total) {
            for (int i = 0; i < CHUNK; ++i)
                chunk[i] = (char)((written + i) * 13);
            size_t want = total - written < CHUNK ? total - written : CHUNK;
            ssize_t n = send(up, chunk, want, MSG_NOSIGNAL);
            if (n <= 0)
                break;
            for (ssize_t i = 0; i < n; ++i)
                sent_sum = sent_sum * 31 + (unsigned char)chunk[i];
            written += n;
        }
        ut_run_for(0.001);
        /* the receiver takes a little every now and then */
        if (turn % 4 == 0) {
            static char buf[32 * 1024];
            ssize_t n = recv(down, buf, sizeof(buf), MSG_DONTWAIT);
            for (ssize_t i = 0; i < n; ++i)
                got_sum = got_sum * 31 + (unsigned char)buf[i];
            if (n > 0)
                received += n;
        }
    }
    fprintf(stderr, "relay: %zu bytes, paused %d times, drained %d times, at most %zu bytes queued (mark %zu)\n",
            received, relay_pauses, drain_count, relay_max_pending, relay_mark);
    CHECK_EQ(received, total);
    CHECK_EQ(relayed, total);
    CHECK_EQ(got_sum, sent_sum);
    CHECK(!relay_overflow);
    CHECK(relay_pauses >= 1);
    CHECK(drain_count >= relay_pauses);
    CHECK(relay_max_pending <= relay_mark + CHUNK);
    CHECK(nw_svr_get_clt(svr, relay_to) != NULL);       /* never dropped */
    CHECK_EQ(close_count, 5);
    relay_to = relay_from = 0;
    close(up);
    close(down);
    ut_run_for(0.1);
    CHECK_EQ(close_count, 7);
    nw_svr_release(svr);

    /*
     * seqpacket: packets wait in the kernel while paused
     */
    char unix_cfg[128];
    ut_unix_cfg(unix_cfg, sizeof(unix_cfg), "seqpacket", "flow.sock");
    recv_count = 0;
    recv_log[0] = 0;
    type.decode_pkg = ut_line_decode;
    svr = ut_svr(unix_cfg, &type, 1024, NULL);
    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", strchr(unix_cfg, '@') + 1);
    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    CHECK_EQ(connect(fd, (struct sockaddr *)&un, sizeof(un)), 0);
    ses = newest();
    pause_each = true;
    for (int i = 1; i <= 4; ++i) {
        char line[8];
        int len = snprintf(line, sizeof(line), "%d\n", i);
        CHECK_EQ(send(fd, line, len, 0), len);
    }
    ut_run_for(0.05);
    CHECK_EQ(recv_count, 1);
    for (int i = 2; i <= 4; ++i) {
        nw_ses_resume_read(ses);
        ut_run_for(0.02);
        CHECK_EQ(recv_count, i);
    }
    CHECK(strcmp(recv_log, "1\n2\n3\n4\n") == 0);
    pause_each = false;
    close(fd);
    nw_svr_release(svr);

    /*
     * nw_clt has the same: pending and on_drain on its session
     */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(in);
    CHECK_EQ(bind(lfd, (struct sockaddr *)&in, len), 0);
    CHECK_EQ(listen(lfd, 4), 0);
    CHECK_EQ(getsockname(lfd, (struct sockaddr *)&in, &len), 0);
    char tcp_cfg[64];
    snprintf(tcp_cfg, sizeof(tcp_cfg), "tcp@127.0.0.1:%d", ntohs(in.sin_port));
    nw_clt_type ct;
    memset(&ct, 0, sizeof(ct));
    ct.decode_pkg = ut_line_decode;
    ct.on_recv_pkg = clt_on_recv_pkg;
    ct.on_connect = clt_on_connect;
    ct.on_drain = clt_on_drain;
    nw_clt *clt = ut_clt(tcp_cfg, &ct, CHUNK, 1.0, NULL);
    nw_ses_pause_read(&clt->ses);                       /* not connected: nothing */
    CHECK(!clt->ses.read_paused);
    CHECK_EQ(nw_clt_start(clt), 0);
    ut_run_for(0.05);
    CHECK_EQ(clt_connected, 1);
    int peer = accept(lfd, NULL, NULL);
    CHECK(peer >= 0);
    sent = 0;
    while (nw_ses_pending(&clt->ses) < 256 * 1024) {
        CHECK_EQ(nw_ses_send(&clt->ses, chunk, CHUNK), 0);
        sent += CHUNK;
    }
    got = 0;
    got_sum = 0;
    for (int i = 0; i < 2000 && got < sent; ++i) {
        got += drain_fd(peer, &got_sum);
        ut_run_for(0.002);
    }
    CHECK_EQ(got, sent);
    CHECK_EQ(clt_drains, 1);
    CHECK_EQ(nw_ses_pending(&clt->ses), 0);
    nw_clt_release(clt);
    close(peer);
    close(lfd);

    return 0;
}
