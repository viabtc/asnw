/*
 * Description: 
 *     History: yang@haipo.me, 2016/04/17, create
 */

# ifndef _NW_JOB_H_
# define _NW_JOB_H_

# include <stdint.h>
# include <stdbool.h>
# include <pthread.h>

# include "nw_evt.h"
# include "nw_buf.h"

typedef struct nw_job_entry {
    uint32_t id;
    void *request;
    void *reply;
    struct nw_job_entry *next;
    struct nw_job_entry *prev;
} nw_job_entry;

typedef struct nw_job_type {
    void *(*on_init)(void);
    void (*on_job)(nw_job_entry *entry, void *privdata);
    void (*on_finish)(nw_job_entry *entry);
    void (*on_cleanup)(nw_job_entry *entry);
    void (*on_release)(void *privdata);
} nw_job_type;

typedef struct nw_job {
    ev_io ev;
    nw_job_type type;
    struct ev_loop *loop;
    int pipefd[2];
    pthread_mutex_t lock;
    pthread_cond_t notify;
    nw_cache *cache;
    int thread_count;
    int thread_start;
    pthread_t *threads;
    bool shutdown;
    nw_job_entry *request_head;
    nw_job_entry *request_tail;
    int request_count;
    nw_job_entry *reply_head;
    nw_job_entry *reply_tail;
    int reply_count;
} nw_job;

nw_job *nw_job_create(nw_job_type *type, int thread_count);
int nw_job_add(nw_job *job, uint32_t id, void *request);
void nw_job_release(nw_job *job);

# endif

