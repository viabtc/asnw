/*
 * nw_dns: names resolved by the system resolver on threads of their own,
 * the answer delivered on the loop thread. Uses only names that need no
 * network: localhost, numeric addresses, and one that can't exist.
 */
# include <pthread.h>
# include <netdb.h>
# include "common.h"
# include "nw_dns.h"

static int answers, failures;
static int last_error;
static nw_addr_t last_addrs[NW_DNS_ADDR_MAX];
static uint32_t last_count;
static pthread_t loop_thread;

static void on_answer(int error, nw_addr_t *addrs, uint32_t count, void *privdata)
{
    CHECK(pthread_equal(pthread_self(), loop_thread));      /* not on a resolver thread */
    answers++;
    last_error = error;
    last_count = count;
    if (error) {
        failures++;
        CHECK(addrs == NULL);
        CHECK_EQ(count, 0);
    } else {
        CHECK(count >= 1 && count <= NW_DNS_ADDR_MAX);
        memcpy(last_addrs, addrs, count * sizeof(nw_addr_t));
    }
    if (privdata)
        (*(int *)privdata)++;
}

static void wait_answers(int want)
{
    for (int i = 0; i < 1000 && answers < want; ++i)
        ut_run_for(0.01);
    CHECK_EQ(answers, want);
}

static bool is_loopback(nw_addr_t *addr)
{
    if (addr->family == AF_INET)
        return addr->addrlen == sizeof(addr->in) && (ntohl(addr->in.sin_addr.s_addr) >> 24) == 127;
    if (addr->family == AF_INET6)
        return addr->addrlen == sizeof(addr->in6) && IN6_IS_ADDR_LOOPBACK(&addr->in6.sin6_addr);
    return false;
}

/* another loop thread has resolver threads of its own, gone with its loop */
static int thread_answers[2];

static void on_thread_answer(int error, nw_addr_t *addrs, uint32_t count, void *privdata)
{
    int *mine = privdata;
    CHECK_EQ(error, 0);
    if (++*mine == 10)
        nw_loop_break();
}

static void *resolving_thread(void *arg)
{
    int *mine = arg;
    for (int i = 0; i < 10; ++i)
        CHECK(nw_dns_resolve("localhost", 2000 + i, on_thread_answer, mine) != NULL);
    nw_timer giveup;
    nw_timer_set(&giveup, 20, false, ut_on_stop, NULL);
    nw_timer_start(&giveup);
    nw_loop_run();
    nw_timer_stop(&giveup);
    CHECK_EQ(*mine, 10);
    /* a request still out when the loop goes: no callback, nothing lost */
    CHECK(nw_dns_resolve("localhost", 1, on_thread_answer, mine) != NULL);
    nw_loop_release();
    return NULL;
}

int main(void)
{
    loop_thread = pthread_self();

    /* a name from /etc/hosts */
    CHECK(nw_dns_resolve("localhost", 8080, on_answer, NULL) != NULL);
    CHECK_EQ(answers, 0);                                   /* never from inside the call */
    wait_answers(1);
    CHECK_EQ(last_error, 0);
    for (uint32_t i = 0; i < last_count; ++i) {
        CHECK(is_loopback(&last_addrs[i]));
        /* the port is in every address (same offset for both families) */
        CHECK_EQ(ntohs(last_addrs[i].in.sin_port), 8080);
    }

    /* numeric addresses resolve to themselves */
    CHECK(nw_dns_resolve("127.0.0.1", 53, on_answer, NULL) != NULL);
    wait_answers(2);
    CHECK_EQ(last_error, 0);
    CHECK_EQ(last_count, 1);
    CHECK_EQ(last_addrs[0].family, AF_INET);
    CHECK_EQ(ntohl(last_addrs[0].in.sin_addr.s_addr), 0x7f000001);
    CHECK_EQ(ntohs(last_addrs[0].in.sin_port), 53);
    CHECK(nw_dns_resolve("::1", 443, on_answer, NULL) != NULL);
    wait_answers(3);
    CHECK_EQ(last_error, 0);
    CHECK_EQ(last_addrs[0].family, AF_INET6);
    CHECK_EQ(last_addrs[0].addrlen, sizeof(last_addrs[0].in6));
    CHECK(IN6_IS_ADDR_LOOPBACK(&last_addrs[0].in6.sin6_addr));
    CHECK_EQ(ntohs(last_addrs[0].in6.sin6_port), 443);

    /* a name that is none */
    CHECK(nw_dns_resolve("not a name", 80, on_answer, NULL) != NULL);
    wait_answers(4);
    CHECK(last_error != 0);
    CHECK_EQ(failures, 1);
    CHECK(strlen(nw_dns_strerror(last_error)) > 0);
    fprintf(stderr, "resolving \"not a name\": %s\n", nw_dns_strerror(last_error));

    /* refused on the spot */
    CHECK(nw_dns_resolve("", 80, on_answer, NULL) == NULL);
    char long_name[NW_DNS_HOST_MAX + 10];
    memset(long_name, 'a', sizeof(long_name));
    long_name[sizeof(long_name) - 1] = 0;
    CHECK(nw_dns_resolve(long_name, 80, on_answer, NULL) == NULL);
    long_name[NW_DNS_HOST_MAX - 1] = 0;                     /* the longest possible: accepted, not found */
    CHECK(nw_dns_resolve(long_name, 80, on_answer, NULL) != NULL);
    wait_answers(5);
    CHECK(last_error != 0);

    /* many at once, half of them cancelled: exactly the others answer */
    int kept = 0, cancelled = 0;
    answers = 0;
    for (int i = 0; i < 60; ++i) {
        nw_dns_req *req = nw_dns_resolve("localhost", i, on_answer, i % 2 ? &cancelled : &kept);
        CHECK(req != NULL);
        if (i % 2)
            nw_dns_cancel(req);
    }
    wait_answers(30);
    ut_run_for(0.1);
    CHECK_EQ(answers, 30);
    CHECK_EQ(kept, 30);
    CHECK_EQ(cancelled, 0);

    /* two more loop threads, each with its own resolver threads */
    pthread_t threads[2];
    for (int i = 0; i < 2; ++i)
        CHECK_EQ(pthread_create(&threads[i], NULL, resolving_thread, &thread_answers[i]), 0);
    for (int i = 0; i < 2; ++i)
        CHECK_EQ(pthread_join(threads[i], NULL), 0);
    CHECK_EQ(thread_answers[0], 10);
    CHECK_EQ(thread_answers[1], 10);

    /* released with the loop; a new loop starts new ones */
    CHECK(nw_dns_resolve("localhost", 1, on_answer, NULL) != NULL);
    nw_loop_release();
    answers = 0;
    CHECK(nw_dns_resolve("localhost", 2, on_answer, NULL) != NULL);
    wait_answers(1);
    nw_loop_release();
    return 0;
}
