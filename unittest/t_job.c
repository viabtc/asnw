/* nw_job: jobs run on workers, results come back on the loop thread */
# include "common.h"

# define JOBS 500
/* ids are 64 bits wide: a connection id of a nw_hub fits */
# define ID_BASE ((uint64_t)7 << 48)

static pthread_t main_thread;
static int finish_count;
static int cleanup_count;
static long sum;

static void *on_init(void)
{
    return malloc(16);
}

static void on_job(nw_job_entry *entry, void *privdata)
{
    CHECK(privdata != NULL);
    CHECK(!pthread_equal(pthread_self(), main_thread));
    /* signals are left to the threads of the application */
    sigset_t blocked;
    CHECK_EQ(pthread_sigmask(SIG_SETMASK, NULL, &blocked), 0);
    CHECK(sigismember(&blocked, SIGTERM) && sigismember(&blocked, SIGINT));
    long n = (long)entry->request;
    entry->result = (void *)(n * n);
}

static void on_finish(nw_job_entry *entry)
{
    CHECK(pthread_equal(pthread_self(), main_thread));
    long n = (long)entry->request;
    CHECK_EQ((long)entry->result, n * n);
    CHECK(entry->id == ID_BASE + n + 1);
    sum += (long)entry->result;
    finish_count++;
}

static void on_cleanup(nw_job_entry *entry)
{
    if (++cleanup_count == JOBS)
        nw_loop_break();
}

static int release_count;

static void on_release(void *privdata)
{
    release_count++;
    free(privdata);
}

int main(void)
{
    main_thread = pthread_self();

    nw_job_type type;
    memset(&type, 0, sizeof(type));
    CHECK(nw_job_create(&type, 4) == NULL);         /* on_job is required */
    type.on_job = on_job;
    type.on_init = on_init;
    CHECK(nw_job_create(&type, 4) == NULL);         /* on_init needs on_release */
    type.on_release = on_release;
    CHECK(nw_job_create(&type, 0) == NULL);         /* at least one worker */
    type.on_finish = on_finish;
    type.on_cleanup = on_cleanup;

    nw_job *job = nw_job_create(&type, 4);
    CHECK(job != NULL);

    long expect = 0;
    for (long i = 0; i < JOBS; ++i) {
        CHECK_EQ(nw_job_add(job, ID_BASE + i + 1, (void *)i), 0);
        expect += i * i;
    }

    ut_run_for(10.0);
    CHECK_EQ(finish_count, JOBS);
    CHECK_EQ(cleanup_count, JOBS);
    CHECK_EQ(sum, expect);

    /* and the calling thread keeps its own signal mask */
    sigset_t blocked;
    CHECK_EQ(pthread_sigmask(SIG_SETMASK, NULL, &blocked), 0);
    CHECK(!sigismember(&blocked, SIGTERM));

    nw_job_release(job);
    CHECK_EQ(release_count, 4);
    return 0;
}
