/*
 * Description: thread pool, runs blocking work off the loop thread
 *     History: yang@haipo.me, 2016/04/17, create
 */

# ifndef _NW_JOB_H_
# define _NW_JOB_H_

# include <stdint.h>
# include <stdbool.h>
# include <pthread.h>

# include "nw_evt.h"
# include "nw_buf.h"

/*
 * nw_job is a thread pool for work that would block the event loop: a
 * database query, a disk read, heavy computation.
 *
 * the life of a job:
 *   any thread      nw_job_add(job, id, request)
 *   worker thread   on_job(entry, privdata)      do the work, set entry->result
 *   loop thread     on_finish(entry)             use the result
 *   loop thread     on_cleanup(entry)            free request and result
 *
 * jobs are started in the order they were added, but with more than one
 * worker they may finish in any order.
 *
 * the loop thread is the thread that created the pool. a program with
 * several loop threads (a nw_hub) can give each thread a pool of its own,
 * or share one: create it on the main thread and call nw_job_add from the
 * others. on_finish then runs on the main thread, which answers by id
 * (nw_hub_send, nw_http_reply_id ...) and must not touch a connection of
 * another thread in any other way.
 *
 * only on_job runs on a worker thread, and the same on_job runs on several
 * threads at once: it must not touch anything else of asnw (no nw_ses_send,
 * no timers, no nw_state) nor unprotected shared data. give each worker its
 * own resources (a database connection ...) with on_init.
 */

typedef struct nw_job_entry {
    /* the id given to nw_job_add, yours to use: the id of the connection
     * waiting for the result (ses->id), or a nw_state id that leads back
     * to it */
    uint64_t id;
    /* the request given to nw_job_add */
    void *request;
    /* the result, set by on_job. NULL until then */
    void *result;
    struct nw_job_entry *next;
} nw_job_entry;

typedef struct nw_job_type {
    /* optional, if set on_release must be set too
     *
     * called once per worker, on the loop thread, inside nw_job_create.
     * the return value is that worker's private data, passed to every
     * on_job it runs. returning NULL makes nw_job_create fail */
    void *(*on_init)(void);
    /* must
     *
     * do the work, on a worker thread. read entry->request, store the
     * result in entry->result. privdata is what on_init returned for this
     * worker, NULL without on_init */
    void (*on_job)(nw_job_entry *entry, void *privdata);
    /* optional
     *
     * called on the loop thread when a job is done, to use entry->result.
     * the entry is only valid during the call */
    void (*on_finish)(nw_job_entry *entry);
    /* optional
     *
     * called on the loop thread right after on_finish, to free
     * entry->request and entry->result. kept apart from on_finish because
     * it is also called, alone, for the jobs that nw_job_release drops:
     * entry->result may then be NULL */
    void (*on_cleanup)(nw_job_entry *entry);
    /* optional
     *
     * called once per worker, on the loop thread, inside nw_job_release,
     * to free what on_init returned */
    void (*on_release)(void *privdata);
} nw_job_type;

/* internal, don't touch the fields */
typedef struct nw_job {
    nw_job_type type;
    struct ev_loop *loop;
    nw_cache *cache;
    int thread_count;
    int thread_start;
    pthread_t *threads;
    /* jobs waiting for a worker */
    pthread_mutex_t request_lock;
    pthread_cond_t notify;
    bool shutdown;
    nw_job_entry *request_head;
    nw_job_entry *request_tail;
    /* jobs done, waiting for the loop thread, which wakeup calls */
    pthread_mutex_t result_lock;
    nw_job_entry *result_head;
    nw_job_entry *result_tail;
    ev_async wakeup;
} nw_job;

/* create a pool of thread_count workers (at least 1) and start them. type
 * is copied. return NULL if on_job is missing, on_init is set without
 * on_release, on_init fails, or a thread can't be created */
nw_job *nw_job_create(nw_job_type *type, int thread_count);

/* queue a job. id and request are handed to the callbacks untouched.
 * return 0, or -1 if out of memory. it may be called from any thread;
 * on_finish and on_cleanup run on the thread that created the pool.
 * the queue has no upper bound: if jobs are added faster than the workers
 * finish them, memory grows */
int nw_job_add(nw_job *job, uint64_t id, void *request);

/* stop the workers and free the pool. it blocks until the jobs being
 * processed right now are done. jobs still queued are not run, and results
 * not delivered yet are not passed to on_finish; on_cleanup is called for
 * each of them so that request and result can be freed. call it from the
 * loop thread, not from a callback of this pool, and only when no other
 * thread can call nw_job_add any more (release the hub first) */
void nw_job_release(nw_job *job);

# endif
