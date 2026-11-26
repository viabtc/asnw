/*
 * Regression (fixed): nw_job_release() used to leak the entry cache and to
 * drop queued requests / undelivered replies without telling anybody.
 * Now every job gets exactly one on_cleanup, whether it finished or not.
 * Leaks are detected by LeakSanitizer (asan build) or valgrind.
 */
# include "common.h"

# define JOBS 50

static int finish_count;
static int cleanup_count;

static void on_job(nw_job_entry *entry, void *privdata)
{
    usleep(2000);
    entry->result = malloc(8);
}

static void on_finish(nw_job_entry *entry)
{
    finish_count++;
}

static void on_cleanup(nw_job_entry *entry)
{
    cleanup_count++;
    free(entry->request);
    free(entry->result);
}

int main(void)
{
    nw_job_type type;
    memset(&type, 0, sizeof(type));
    type.on_job = on_job;
    type.on_finish = on_finish;
    type.on_cleanup = on_cleanup;

    /* release after everything was delivered */
    nw_job *job = nw_job_create(&type, 2);
    CHECK(job != NULL);
    for (int i = 0; i < 10; ++i)
        CHECK_EQ(nw_job_add(job, i, malloc(8)), 0);
    ut_run_for(0.5);
    CHECK_EQ(finish_count, 10);
    CHECK_EQ(cleanup_count, 10);
    nw_job_release(job);
    CHECK_EQ(cleanup_count, 10);

    /* release with most of the work still queued or in flight */
    finish_count = cleanup_count = 0;
    job = nw_job_create(&type, 2);
    CHECK(job != NULL);
    for (int i = 0; i < JOBS; ++i)
        CHECK_EQ(nw_job_add(job, i, malloc(8)), 0);
    ut_run_for(0.01);
    nw_job_release(job);
    fprintf(stderr, "%d jobs: %d finished, %d cleaned up\n", JOBS, finish_count, cleanup_count);
    CHECK(finish_count < JOBS);
    CHECK_EQ(cleanup_count, JOBS);
    return 0;
}
