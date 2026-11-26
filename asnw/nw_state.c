/*
 * Description: table of pending states with timeout
 *     History: yang@haipo.me, 2016/03/22, create
 */

# include <stdlib.h>
# include <string.h>
# include <stdbool.h>

# include "nw_state.h"

nw_state *nw_state_create(nw_state_type *type, uint32_t data_size)
{
    if (type->on_timeout == NULL)
        return NULL;

    nw_loop_init();
    nw_state *state = malloc(sizeof(nw_state));
    if (state == NULL) {
        return NULL;
    }
    memset(state, 0, sizeof(nw_state));
    state->loop = nw_default_loop;
    state->type = *type;
    state->data_size = data_size;
    state->cache = nw_cache_create(sizeof(nw_state_entry) + data_size);
    if (state->cache == NULL) {
        free(state);
        return NULL;
    }
    state->dict = nw_dict_create(0);
    if (state->dict == NULL) {
        nw_cache_release(state->cache);
        free(state);
        return NULL;
    }

    return state;
}

static void state_release(nw_state *state, nw_state_entry *entry)
{
    if (state->type.on_release)
        state->type.on_release(entry);
    nw_cache_free(state->cache, entry);
}

static void on_timeout(struct ev_loop *loop, ev_timer *ev, int events)
{
    nw_state_entry *entry = (nw_state_entry *)ev;
    nw_state *state = entry->state;
    /* take it out of the dict first: whatever the callback does with this
     * id (del, mod, add it again) can no longer touch the entry being
     * timed out */
    nw_dict_del(state->dict, entry->id);
    state->type.on_timeout(entry);
    state_release(state, entry);
}

static uint32_t get_available_id(nw_state *state)
{
    while (true) {
        state->id_start++;
        if (state->id_start == 0)
            state->id_start++;
        if (nw_state_get(state, state->id_start) == NULL)
            break;
    }
    return state->id_start;
}

nw_state_entry *nw_state_add(nw_state *state, double timeout, uint32_t id)
{
    if (id != 0 && nw_state_get(state, id) != NULL)
        return NULL;
    if (nw_dict_count(state->dict) == UINT32_MAX)
        return NULL;
    nw_state_entry *entry = nw_cache_alloc(state->cache);
    if (entry == NULL) {
        return NULL;
    }

    if (id != 0) {
        entry->id = id;
    } else {
        entry->id = get_available_id(state);
    }
    if (nw_dict_add(state->dict, entry->id, entry) < 0) {
        nw_cache_free(state->cache, entry);
        return NULL;
    }
    ev_timer_init(&entry->ev, on_timeout, timeout, 0);
    ev_timer_start(state->loop, &entry->ev);
    entry->state = state;
    entry->data = ((void *)entry + sizeof(nw_state_entry));
    memset(entry->data, 0, state->data_size);

    return entry;
}

nw_state_entry *nw_state_get(nw_state *state, uint32_t id)
{
    return nw_dict_get(state->dict, id);
}

int nw_state_set_timeout(nw_state *state, uint32_t id, double timeout)
{
    nw_state_entry *entry = nw_state_get(state, id);
    if (entry == NULL)
        return -1;
    ev_timer_stop(state->loop, &entry->ev);
    ev_timer_set(&entry->ev, timeout, 0);
    ev_timer_start(state->loop, &entry->ev);

    return 0;
}

int nw_state_del(nw_state *state, uint32_t id)
{
    nw_state_entry *entry = nw_state_get(state, id);
    if (entry == NULL)
        return -1;
    ev_timer_stop(state->loop, &entry->ev);
    nw_dict_del(state->dict, id);
    state_release(state, entry);

    return 0;
}

size_t nw_state_count(nw_state *state)
{
    return nw_dict_count(state->dict);
}

void nw_state_release(nw_state *state)
{
    nw_dict_iterator iter;
    nw_dict_entry *item;
    nw_dict_iter_init(&iter, state->dict);
    while ((item = nw_dict_next(&iter)) != NULL) {
        nw_state_entry *entry = item->val;
        ev_timer_stop(state->loop, &entry->ev);
        state_release(state, entry);
    }
    nw_dict_release(state->dict);
    nw_cache_release(state->cache);
    free(state);
}

void nw_state_iter_init(nw_state_iterator *iter, nw_state *state)
{
    nw_dict_iter_init(&iter->iter, state->dict);
}

nw_state_entry *nw_state_next(nw_state_iterator *iter)
{
    nw_dict_entry *item = nw_dict_next(&iter->iter);
    return item ? item->val : NULL;
}
