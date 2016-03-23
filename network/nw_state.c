/*
 * Description: 
 *     History: yang@haipo.me, 2016/03/22, create
 */

# include <stdlib.h>
# include <stdbool.h>

# include "nw_state.h"

# define NW_STATE_HASH_TABLE_INIT_SIZE 64

nw_state *nw_state_create(nw_state_timeout_callback timeout_callback)
{
    nw_loop_init();
    nw_state *context = malloc(sizeof(nw_state));
    if (context == NULL) {
        return NULL;
    }
    memset(context, 0, sizeof(nw_state));
    context->table_size = NW_STATE_HASH_TABLE_INIT_SIZE;
    context->table_mask = context->table_size - 1;
    context->table = calloc(context->table_size, sizeof(nw_state_entry *));
    if (context->table == NULL) {
        free(context);
        return NULL;
    }
    context->timeout_callback = timeout_callback;
    context->loop = nw_default_loop;
    for (int i = 0; i < NW_STATE_CACHE_NUM; ++i) {
        context->caches[i] = nw_cache_create(2 << (i + 2));
        if (context->caches[i] == NULL) {
            nw_state_release(context);
            return NULL;
        }
    }

    return context;
}

static nw_cache *cache_choice(nw_state *context, size_t size)
{
    for (int i = 0; i < NW_STATE_CACHE_NUM; ++i) {
        if (size < (2 << (i + 2))) {
            return context->caches[i];
        }
    }
    return NULL;
}

static void *cache_alloc(nw_state *context, uint32_t data_size)
{
    size_t real_size = sizeof(nw_state_entry) + data_size;
    nw_cache *cache = cache_choice(context, real_size);
    if (cache == NULL) {
        return malloc(real_size);
    }
    return nw_cache_alloc(cache);
}

static void cache_free(nw_state *context, nw_state_entry *entry)
{
    size_t real_size = sizeof(nw_state_entry) + entry->data_size;
    nw_cache *cache = cache_choice(context, real_size);
    if (cache == NULL) {
        free(entry);
    }
    nw_cache_free(cache, entry);
}

static int state_expand_if_needed(nw_state *context)
{
    if (context->used < context->table_size)
        return 0;

    uint32_t new_table_size = context->table_size * 2;
    uint32_t new_table_mask = new_table_size - 1;
    nw_state_entry **new_table = calloc(new_table_size, sizeof(nw_state_entry *));
    if (new_table == NULL) {
        return -1;
    }

    for (uint32_t i = 0; i < context->table_size; ++i) {
        nw_state_entry *entry = context->table[i];
        nw_state_entry *next = NULL;
        while (entry) {
            next = entry->next;
            uint32_t index = entry->id & new_table_mask;
            entry->next = new_table[index];
            new_table[index] = entry;
            entry = next;
        }
    }

    free(context->table);
    context->table = new_table;
    context->table_size = new_table_size;
    context->table_mask = new_table_mask;

    return 0;
}

static int state_remove(nw_state *context, nw_state_entry *entry)
{
    uint32_t index = entry->id & context->table_mask;
    nw_state_entry *curr = context->table[index];
    nw_state_entry *prev = NULL;
    while (curr) {
        if (curr->id == entry->id) {
            if (prev) {
                prev->next = entry->next;
            } else {
                context->table[index] = entry->next;
            }
            cache_free(context, entry);
            context->used--;
            return 1;
        }
        prev = curr;
        curr = curr->next;
    }

    return 0;
}

static void on_timeout(struct ev_loop *loop, ev_timer *ev, int events)
{
    nw_state_entry *entry = (nw_state_entry *)ev;
    nw_state *context = entry->context;
    context->timeout_callback(entry);
    state_remove(context, entry);
}

nw_state_entry *nw_state_add(nw_state *context, uint32_t size, double timeout)
{
    if (context->used == UINT32_MAX)
        return NULL;
    nw_state_entry *entry = cache_alloc(context, size);
    if (entry == NULL) {
        return NULL;
    }

    entry->id = ++context->id;
    if (entry->id == 0)
        entry->id = ++context->id;
    entry->context = context;
    entry->data_size = size;
    ev_timer_init(&entry->ev, on_timeout, timeout, 0);
    ev_timer_start(context->loop, &entry->ev);

    state_expand_if_needed(context);
    uint32_t index = entry->id & context->table_mask;
    entry->next = context->table[index];
    context->table[index] = entry;
    context->used++;

    return entry;
}

nw_state_entry *nw_state_get(nw_state *context, uint32_t id)
{
    uint32_t index = id & context->table_mask;
    nw_state_entry *entry = context->table[index];
    while (entry) {
        if (entry->id == id)
            return entry;
        entry = entry->next;
    }

    return NULL;
}

int nw_state_mod(nw_state *context, uint32_t id, double timeout)
{
    nw_state_entry *entry = nw_state_get(context, id);
    if (entry == NULL)
        return -1;
    ev_timer_stop(context->loop, &entry->ev);
    ev_timer_set(&entry->ev, timeout, 0);
    ev_timer_start(context->loop, &entry->ev);

    return 0;
}

int nw_state_del(nw_state *context, uint32_t id)
{
    nw_state_entry *entry = nw_state_get(context, id);
    if (entry == NULL)
        return -1;
    ev_timer_stop(context->loop, &entry->ev);
    state_remove(context, entry);

    return 0;
}

size_t nw_state_count(nw_state *context)
{
    return context->used;
}

void nw_state_release(nw_state *context)
{
    for (uint32_t i = 0; i < context->table_size; ++i) {
        nw_state_entry *entry = context->table[i];
        nw_state_entry *next = NULL;
        while (entry) {
            next = entry->next;
            ev_timer_stop(context->loop, &entry->ev);
            cache_free(context, entry);
            entry = next;
        }
    }
    free(context->table);
    for (int i = 0; i < NW_STATE_CACHE_NUM; ++i) {
        if (context->caches[i]) {
            nw_cache_release(context->caches[i]);
        }
    }
    free(context);
}

nw_state_iterator *nw_state_get_iterator(nw_state *context)
{
    nw_state_iterator *iter = malloc(sizeof(nw_state_iterator));
    if (iter == NULL)
        return NULL;
    memset(iter, 0, sizeof(nw_state_iterator));
    iter->context = context;
    iter->index = -1;
    iter->entry = NULL;
    iter->next_entry = NULL;

    return iter;
}

nw_state_entry *nw_state_next(nw_state_iterator *iter)
{
    while (true) {
        if (iter->entry == NULL) {
            iter->index++;
            if (iter->index >= iter->context->table_size)
                break;
            iter->entry = iter->context->table[iter->index];
        } else {
            iter->entry = iter->next_entry;
        }
        if (iter->entry) {
            iter->next_entry = iter->entry->next;
            return iter->entry;
        }
    }

    return NULL;
}

void nw_state_iterator_release(nw_state_iterator *iter)
{
    free(iter);
}

