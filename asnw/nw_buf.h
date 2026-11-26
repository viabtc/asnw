/*
 * Description: buffers: fixed size buf, buf pool, buf list and object cache
 *     History: yang@haipo.me, 2016/03/16, create
 */

# ifndef _NW_BUF_H_
# define _NW_BUF_H_

# include <stdint.h>
# include <stdlib.h>

/*
 * these are the building blocks nw_ses uses for its read buffer and pending
 * send list. you only need them directly to share one nw_buf_pool between
 * several clients (nw_clt_cfg.buf_pool) or to use nw_cache for your own
 * fixed size objects. none of it is thread safe.
 */

/* nw_buf is a fixed size buffer. the valid data is data[rpos, wpos) */
typedef struct nw_buf {
    /* capacity of data */
    uint32_t size;
    /* read position: start of the data not consumed yet */
    uint32_t rpos;
    /* write position: end of the data, new data goes here */
    uint32_t wpos;
    /* next buf when linked in a nw_buf_list */
    struct nw_buf *next;
    char data[];
} nw_buf;

/*
 * nw_buf_pool is a factory of nw_buf of one size. freed bufs are kept for
 * reuse: the pool never gives memory back to the system by itself, so it
 * keeps the memory of its busiest moment until nw_buf_pool_release, or
 * until the program asks for it with nw_buf_pool_shrink.
 *
 * that is on purpose, here and in nw_cache: the memory of something that
 * was freed stays readable. the library counts on it - a session that was
 * closed inside a callback is still asked whether it is closed, a callback
 * that closed its own connection may still be reading its message, an
 * iterator steps off a deleted entry. don't make them free on their own;
 * nw_buf_pool_shrink is safe because the program calls it, at a moment
 * when nothing of the kind is going on.
 */

/* the free bufs nw_buf_pool_shrink always leaves in a pool */
# define NW_BUF_POOL_MIN_FREE 64

typedef struct nw_buf_pool {
    /* capacity of every buf of this pool */
    uint32_t size;
    /* number handed out and not given back yet */
    uint32_t used;
    /* number of bufs waiting for reuse in free_arr */
    uint32_t free;
    /* capacity of free_arr, grows on demand */
    uint32_t free_total;
    nw_buf **free_arr;
} nw_buf_pool;

/* nw_buf_list is a fifo of nw_buf from one pool: data is added at the tail
 * and consumed from the head */
typedef struct nw_buf_list {
    nw_buf_pool *pool;
    /* number of bufs in the list */
    uint32_t count;
    /* max number of bufs, 0 for no limit. the list then holds at most
     * limit * pool->size bytes */
    uint32_t limit;
    nw_buf *head;
    nw_buf *tail;
} nw_buf_list;

/* nw_cache is a factory of untyped objects of one size, with the same reuse
 * behaviour as nw_buf_pool */
typedef struct nw_cache {
    /* size of every object */
    uint32_t size;
    /* number handed out and not given back yet */
    uint32_t used;
    /* number of objects waiting for reuse in free_arr */
    uint32_t free;
    /* capacity of free_arr, grows on demand */
    uint32_t free_total;
    void **free_arr;
} nw_cache;

/* nw_buf operation */

/* bytes of data in the buf: wpos - rpos */
size_t nw_buf_size(nw_buf *buf);
/* bytes that can still be written: size - wpos */
size_t nw_buf_avail(nw_buf *buf);
/* copy data to the end of the buf. return the bytes copied, less than size
 * if the buf is full */
size_t nw_buf_write(nw_buf *buf, const void *data, size_t size);
/* move the data to the start of the buf so that the space of the consumed
 * part can be written again */
void nw_buf_shift(nw_buf *buf);

/* nw_buf_pool operation */

/* create a pool of bufs with `size` bytes of data each. NULL if size is 0
 * or out of memory */
nw_buf_pool *nw_buf_pool_create(uint32_t size);
/* get an empty buf, reused or newly allocated. NULL if out of memory */
nw_buf *nw_buf_alloc(nw_buf_pool *pool);
/* give a buf back to the pool it was allocated from */
void nw_buf_free(nw_buf_pool *pool, nw_buf *buf);
/* free the pool and the bufs waiting for reuse. bufs still in use must be
 * given back first */
void nw_buf_pool_release(nw_buf_pool *pool);
/*
 * give the free bufs a pool has no use for back to the system. whether
 * and when is for the program to decide: after a peak of traffic, every
 * few minutes, never.
 *
 * the pool keeps as many free bufs as it has bufs in use, and never less
 * than NW_BUF_POOL_MIN_FREE; what it has beyond that is freed. a pool at
 * work is therefore left with what it needs, and a small one is left
 * alone. return the number of bufs freed.
 *
 * call it on the thread the pool belongs to, from a timer of your own or
 * from nw_hub_run, not from a callback of a server or a client: a
 * callback that closed its connection may still be reading the message it
 * was given, out of a buf that is free by then. the pool of a server is
 * svr->buf_pool, that of a client clt->buf_pool.
 */
uint32_t nw_buf_pool_shrink(nw_buf_pool *pool);

/* nw_buf_list operation */

/* create an empty list taking its bufs from pool, at most `limit` of them
 * (0: no limit). NULL if out of memory */
nw_buf_list *nw_buf_list_create(nw_buf_pool *pool, uint32_t limit);
/* stream write: fill the tail buf, then add bufs as needed, data may be
 * split across bufs. return the bytes written, less than size if the limit
 * is reached or memory runs out (what was written stays in the list) */
size_t nw_buf_list_write(nw_buf_list *list, const void *data, size_t size);
/* packet write: put data in a new buf of its own, never split. return size,
 * or 0 if size is bigger than the buf size, the limit is reached or memory
 * runs out */
size_t nw_buf_list_append(nw_buf_list *list, const void *data, size_t size);
/* remove the head buf, if any, and give it back to the pool */
void nw_buf_list_shift(nw_buf_list *list);
/* free the list and give all its bufs back to the pool */
void nw_buf_list_release(nw_buf_list *list);

/* nw_cache operation */

/* create a cache of objects of `size` bytes. NULL if out of memory */
nw_cache *nw_cache_create(uint32_t size);
/* get an object, reused or newly allocated. the content is NOT zeroed.
 * NULL if out of memory */
void *nw_cache_alloc(nw_cache *cache);
/* give an object back to the cache it was allocated from */
void nw_cache_free(nw_cache *cache, void *obj);
/* free the cache and the objects waiting for reuse. objects still in use
 * must be given back first */
void nw_cache_release(nw_cache *cache);

# endif
