/* nw_buf / nw_buf_pool / nw_buf_list / nw_cache */
# include "common.h"

int main(void)
{
    nw_buf_pool *pool = nw_buf_pool_create(8);
    CHECK(pool != NULL);

    nw_buf *buf = nw_buf_alloc(pool);
    CHECK(buf != NULL);
    CHECK_EQ(nw_buf_size(buf), 0);
    CHECK_EQ(nw_buf_avail(buf), 8);
    CHECK_EQ(nw_buf_write(buf, "abcdef", 6), 6);
    CHECK_EQ(nw_buf_write(buf, "ghij", 4), 2);      /* truncated to capacity */
    CHECK_EQ(nw_buf_avail(buf), 0);
    buf->rpos = 3;
    nw_buf_shift(buf);
    CHECK_EQ(buf->rpos, 0);
    CHECK_EQ(buf->wpos, 5);
    CHECK(memcmp(buf->data, "defgh", 5) == 0);
    buf->rpos = buf->wpos;
    nw_buf_shift(buf);
    CHECK_EQ(buf->wpos, 0);

    /* a freed buf is reused, and comes back clean */
    buf->wpos = 4;
    nw_buf_free(pool, buf);
    nw_buf *again = nw_buf_alloc(pool);
    CHECK(again == buf);
    CHECK_EQ(again->wpos, 0);
    nw_buf_free(pool, again);

    /* free list grows past its initial 64 slots */
    nw_buf *many[200];
    for (int i = 0; i < 200; ++i)
        CHECK((many[i] = nw_buf_alloc(pool)) != NULL);
    CHECK_EQ(pool->used, 200);
    for (int i = 0; i < 200; ++i)
        nw_buf_free(pool, many[i]);
    CHECK_EQ(pool->free, 200);
    CHECK_EQ(pool->used, 0);

    /* nw_buf_pool_shrink. nothing in use: the minimum stays */
    CHECK_EQ(nw_buf_pool_shrink(pool), 200 - NW_BUF_POOL_MIN_FREE);
    CHECK_EQ(pool->free, NW_BUF_POOL_MIN_FREE);
    CHECK_EQ(nw_buf_pool_shrink(pool), 0);
    /* as many free bufs are kept as there are in use */
    nw_buf *more[300];
    for (int i = 0; i < 300; ++i)
        CHECK((more[i] = nw_buf_alloc(pool)) != NULL);
    CHECK_EQ(pool->free, 0);
    CHECK_EQ(nw_buf_pool_shrink(pool), 0);
    for (int i = 0; i < 200; ++i)
        nw_buf_free(pool, more[i]);
    CHECK_EQ(pool->used, 100);
    CHECK_EQ(nw_buf_pool_shrink(pool), 100);
    CHECK_EQ(pool->free, 100);
    /* less in use than the minimum: the minimum */
    for (int i = 200; i < 260; ++i)
        nw_buf_free(pool, more[i]);
    CHECK_EQ(pool->used, 40);
    CHECK_EQ(pool->free, 160);
    CHECK_EQ(nw_buf_pool_shrink(pool), 160 - NW_BUF_POOL_MIN_FREE);
    for (int i = 260; i < 300; ++i)
        nw_buf_free(pool, more[i]);
    CHECK_EQ(nw_buf_pool_shrink(pool), 40);
    CHECK_EQ(pool->free, NW_BUF_POOL_MIN_FREE);
    CHECK_EQ(pool->used, 0);
    /* and the pool goes on as before: the bufs kept are used first */
    again = nw_buf_alloc(pool);
    CHECK(again != NULL);
    CHECK_EQ(pool->free, NW_BUF_POOL_MIN_FREE - 1);
    CHECK_EQ(again->size, 8);
    nw_buf_free(pool, again);

    /* stream write spans bufs and honours the limit */
    nw_buf_list *list = nw_buf_list_create(pool, 3);
    CHECK(list != NULL);
    CHECK_EQ(nw_buf_list_write(list, "0123456789", 10), 10);
    CHECK_EQ(list->count, 2);
    CHECK_EQ(nw_buf_list_write(list, "abcdefghijklmnopqrstuvwxyz", 26), 14); /* 24 bytes max */
    CHECK_EQ(list->count, 3);
    CHECK(memcmp(list->head->data, "01234567", 8) == 0);
    CHECK(memcmp(list->head->next->data, "89abcdef", 8) == 0);
    CHECK(memcmp(list->tail->data, "ghijklmn", 8) == 0);
    nw_buf_list_shift(list);
    CHECK_EQ(list->count, 2);
    nw_buf_list_shift(list);
    nw_buf_list_shift(list);
    CHECK_EQ(list->count, 0);
    CHECK(list->head == NULL && list->tail == NULL);
    nw_buf_list_shift(list);                        /* no-op on empty list */

    /* packet append: one packet per buf, oversize rejected, limit honoured */
    CHECK_EQ(nw_buf_list_append(list, "123456789", 9), 0);
    CHECK_EQ(list->count, 0);
    CHECK_EQ(nw_buf_list_append(list, "abc", 3), 3);
    CHECK_EQ(nw_buf_list_append(list, "de", 2), 2);
    CHECK_EQ(nw_buf_list_append(list, "f", 1), 1);
    CHECK_EQ(nw_buf_list_append(list, "g", 1), 0);
    CHECK_EQ(nw_buf_size(list->head), 3);
    CHECK_EQ(nw_buf_size(list->tail), 1);
    nw_buf_list_release(list);
    nw_buf_pool_release(pool);

    nw_cache *cache = nw_cache_create(32);
    CHECK(cache != NULL);
    void *objs[200];
    for (int i = 0; i < 200; ++i) {
        CHECK((objs[i] = nw_cache_alloc(cache)) != NULL);
        memset(objs[i], i, 32);
    }
    for (int i = 0; i < 200; ++i)
        nw_cache_free(cache, objs[i]);
    CHECK_EQ(cache->free, 200);
    CHECK(nw_cache_alloc(cache) == objs[199]);
    nw_cache_free(cache, objs[199]);
    nw_cache_release(cache);

    return 0;
}
