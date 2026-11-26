/*
 * One nw_job shared by several threads: jobs are added from threads other
 * than the one that created the pool, at the same time as that thread
 * delivers results and adds jobs itself. Every job is run once and its
 * result comes back on the thread of the pool.
 */
# include <pthread.h>
# include "common.h"
# include "nw_job.h"

# define ADDERS 4
# define PER_ADDER 5000
# define FOLLOW_UPS 1000
# define TOTAL (ADDERS * PER_ADDER + FOLLOW_UPS)

static nw_job *job;
static pthread_t pool_thread;
static uint8_t seen[TOTAL];
static int ran, finished, cleaned, follow_ups;

static void on_job(nw_job_entry *entry, void *privdata)
{
    __sync_fetch_and_add(&ran, 1);
    entry->result = (void *)(uintptr_t)(entry->id * 3 + 1);
}

static void on_finish(nw_job_entry *entry)
{
    CHECK(pthread_equal(pthread_self(), pool_thread));
    CHECK(entry->id < TOTAL);
    CHECK_EQ((uintptr_t)entry->result, entry->id * 3 + 1);
    CHECK_EQ((uintptr_t)entry->request, entry->id + 7);
    CHECK_EQ(seen[entry->id], 0);
    seen[entry->id] = 1;
    finished++;
    /* the pool thread adds jobs too, from inside a callback */
    if (follow_ups < FOLLOW_UPS) {
        uint64_t id = ADDERS * PER_ADDER + follow_ups++;
        CHECK_EQ(nw_job_add(job, id, (void *)(uintptr_t)(id + 7)), 0);
    }
}

static void on_cleanup(nw_job_entry *entry)
{
    CHECK(pthread_equal(pthread_self(), pool_thread));
    cleaned++;
}

static void *adder(void *arg)
{
    uint64_t base = (uintptr_t)arg * PER_ADDER;
    for (uint64_t i = 0; i < PER_ADDER; ++i) {
        uint64_t id = base + i;
        if (nw_job_add(job, id, (void *)(uintptr_t)(id + 7)) != 0)
            abort();
        if (i % 512 == 0)
            usleep(1000);
    }
    return NULL;
}

int main(void)
{
    pool_thread = pthread_self();
    nw_job_type type;
    memset(&type, 0, sizeof(type));
    type.on_job = on_job;
    type.on_finish = on_finish;
    type.on_cleanup = on_cleanup;
    job = nw_job_create(&type, 3);
    CHECK(job != NULL);

    pthread_t threads[ADDERS];
    for (uintptr_t i = 0; i < ADDERS; ++i)
        CHECK_EQ(pthread_create(&threads[i], NULL, adder, (void *)i), 0);
    for (int i = 0; i < 3000 && finished < TOTAL; ++i)
        ut_run_for(0.01);
    for (int i = 0; i < ADDERS; ++i)
        CHECK_EQ(pthread_join(threads[i], NULL), 0);

    CHECK_EQ(finished, TOTAL);
    CHECK_EQ(cleaned, TOTAL);
    CHECK_EQ(ran, TOTAL);
    for (int i = 0; i < TOTAL; ++i)
        CHECK_EQ(seen[i], 1);
    /* every entry went back to the cache */
    CHECK_EQ(job->cache->used, 0);

    /* jobs added from another thread and never run are cleaned up by release */
    nw_job_release(job);
    nw_loop_release();
    return 0;
}
