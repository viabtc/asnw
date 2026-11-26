/*
 * nw_svr_get_clt / nw_svr_send: getting back to a connection by id after
 * asynchronous work. Shows why the id and not the pointer must be kept:
 * the nw_ses memory of a closed connection is reused by the next one.
 */
# include "common.h"

static nw_svr *svr;
static uint64_t last_id;
static nw_ses *last_ses;

static void on_recv_pkg(nw_ses *ses, void *data, size_t size)
{
    /* a request arrives; the answer will be produced later */
    last_id = ses->id;
    last_ses = ses;
}

static int tcp_connect(nw_addr *addr)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    CHECK_EQ(connect(fd, NW_SOCKADDR(addr), sizeof(addr->in)), 0);
    return fd;
}

int main(void)
{
    nw_svr_type type;
    memset(&type, 0, sizeof(type));
    type.decode_pkg = ut_line_decode;
    type.on_recv_pkg = on_recv_pkg;
    svr = ut_svr("tcp@127.0.0.1:0", &type, 1024, NULL);
    nw_addr addr;
    CHECK_EQ(nw_sock_host_addr(svr->bind_list[0].sockfd, &addr), 0);

    CHECK(nw_svr_get_clt(svr, 0) == NULL);
    CHECK(nw_svr_get_clt(svr, 1) == NULL);
    CHECK_EQ(nw_svr_send(svr, 1, "x\n", 2), -1);

    /* client a asks, and is still there when the answer is ready */
    char buf[16];
    int a = tcp_connect(&addr);
    CHECK_EQ(write(a, "req\n", 4), 4);
    ut_run_for(0.05);
    uint64_t a_id = last_id;
    nw_ses *a_ses = last_ses;
    CHECK(a_id != 0);
    CHECK(nw_svr_get_clt(svr, a_id) == a_ses);
    CHECK_EQ(nw_svr_send(svr, a_id, "answer\n", 7), 0);
    CHECK_EQ(read(a, buf, sizeof(buf)), 7);

    /* client a leaves before its second answer */
    CHECK_EQ(write(a, "req\n", 4), 4);
    ut_run_for(0.05);
    close(a);
    ut_run_for(0.05);
    CHECK_EQ(svr->clt_count, 0);
    CHECK(nw_svr_get_clt(svr, a_id) == NULL);
    CHECK_EQ(nw_svr_send(svr, a_id, "answer\n", 7), -1);

    /* client b connects and gets the session memory a had: a pointer kept
     * for a would now reach b, the id does not */
    int b = tcp_connect(&addr);
    CHECK_EQ(write(b, "req\n", 4), 4);
    ut_run_for(0.05);
    uint64_t b_id = last_id;
    CHECK(last_ses == a_ses);
    CHECK(b_id != a_id);
    CHECK(nw_svr_get_clt(svr, a_id) == NULL);
    CHECK(nw_svr_get_clt(svr, b_id) == a_ses);
    CHECK_EQ(nw_svr_send(svr, a_id, "for a\n", 6), -1);
    CHECK_EQ(recv(b, buf, sizeof(buf), MSG_DONTWAIT), -1);      /* b got nothing meant for a */

    /* closing from the server side removes the id as well */
    nw_svr_close_clt(svr, nw_svr_get_clt(svr, b_id));
    CHECK(nw_svr_get_clt(svr, b_id) == NULL);
    close(b);

    /* many connections: every id resolves to its own session */
    enum { N = 300 };
    int fds[N];
    for (int i = 0; i < N; ++i)
        fds[i] = tcp_connect(&addr);
    ut_run_for(0.2);
    CHECK_EQ(svr->clt_count, N);
    CHECK_EQ(nw_dict_count(svr->clt_dict), N);
    /* the iterator visits every connection exactly once */
    nw_svr_iterator iter;
    nw_ses *ses;
    int listed = 0;
    uint64_t id_sum = 0, expect_sum = 0;
    nw_svr_iter_init(&iter, svr);
    while ((ses = nw_svr_next(&iter)) != NULL) {
        CHECK(nw_svr_get_clt(svr, ses->id) == ses);
        id_sum += ses->id;
        listed++;
    }
    CHECK_EQ(listed, N);
    CHECK(nw_svr_next(&iter) == NULL);
    for (uint64_t id = b_id + 1; id <= b_id + N; ++id)
        expect_sum += id;
    CHECK_EQ(id_sum, expect_sum);

    /* the connection just returned may be closed while iterating: this is
     * the loop a heartbeat or idle check runs */
    int closed = 0;
    listed = 0;
    nw_svr_iter_init(&iter, svr);
    while ((ses = nw_svr_next(&iter)) != NULL) {
        listed++;
        if (ses->id % 3 == 0) {
            nw_svr_close_clt(svr, ses);
            closed++;
        }
    }
    CHECK_EQ(listed, N);
    CHECK_EQ(closed, N / 3);
    CHECK_EQ(svr->clt_count, N - closed);
    listed = 0;
    nw_svr_iter_init(&iter, svr);
    while ((ses = nw_svr_next(&iter)) != NULL) {
        CHECK(ses->id % 3 != 0);
        listed++;
    }
    CHECK_EQ(listed, N - closed);

    /* clients hanging up are taken out as well */
    ut_run_for(0.05);
    int still_open = 0;
    for (int i = 0; i < N; i += 2) {
        if (nw_svr_get_clt(svr, b_id + 1 + i) != NULL)
            still_open++;
        close(fds[i]);
    }
    ut_run_for(0.2);
    CHECK_EQ(svr->clt_count, N - closed - still_open);
    CHECK_EQ(nw_dict_count(svr->clt_dict), svr->clt_count);

    CHECK(svr->clt_count > 50);
    nw_svr_release(svr);            /* with the rest still connected */
    for (int i = 1; i < N; i += 2)
        close(fds[i]);
    return 0;
}
