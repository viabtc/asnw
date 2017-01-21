/*
 * Description: network buf manager
 *     History: yang@haipo.me, 2016/03/16, create
 */

# ifndef _NW_BUF_H_
# define _NW_BUF_H_

# include <stdint.h>
# include <stdlib.h>

typedef struct nw_buf {
    uint32_t size;
    uint32_t rpos;
    uint32_t wpos;
    struct nw_buf *next;
    char data[];
} nw_buf;

typedef struct nw_buf_pool {
    uint32_t size;
    uint32_t used;
    uint32_t free;
    uint32_t free_total;
    nw_buf **free_arr;
} nw_buf_pool;

typedef struct nw_buf_list {
    nw_buf_pool *pool;
    uint32_t count;
    uint32_t limit;
    nw_buf *head;
    nw_buf *tail;
} nw_buf_list;

typedef struct nw_cache {
    uint32_t size;
    uint32_t used;
    uint32_t free;
    uint32_t free_total;
    void **free_arr;
} nw_cache;

size_t nw_buf_size(nw_buf *buf);
size_t nw_buf_avail(nw_buf *buf);
size_t nw_buf_write(nw_buf *buf, const void *data, size_t len);
void nw_buf_shift(nw_buf *buf);

nw_buf_pool *nw_buf_pool_create(uint32_t size);
nw_buf *nw_buf_alloc(nw_buf_pool *pool);
void nw_buf_free(nw_buf_pool *pool, nw_buf *buf);
void nw_buf_pool_release(nw_buf_pool *pool);

nw_buf_list *nw_buf_list_create(nw_buf_pool *pool, uint32_t limit);
size_t nw_buf_list_write(nw_buf_list *list, const void *data, size_t len);
size_t nw_buf_list_append(nw_buf_list *list, const void *data, size_t len);
void nw_buf_list_shift(nw_buf_list *list);
void nw_buf_list_release(nw_buf_list *list);

nw_cache *nw_cache_create(uint32_t size);
void *nw_cache_alloc(nw_cache *cache);
void nw_cache_free(nw_cache *cache, void *obj);
void nw_cache_release(nw_cache *cache);

# endif

