/*
 * Description: table of pending states with timeout
 *     History: yang@haipo.me, 2016/03/22, create
 */

# ifndef _NW_STATE_H_
# define _NW_STATE_H_

# include <stdint.h>
# include <stddef.h>

# include "nw_evt.h"
# include "nw_buf.h"
# include "nw_dict.h"

/*
 * nw_state keeps track of things you are waiting for, each with a timeout.
 *
 * the typical use is matching replies to requests: when sending a request,
 * nw_state_add an entry, put what you need to continue later in its data
 * and send the entry id along as the sequence number. when the reply comes
 * back, nw_state_get the entry by that id, continue, and nw_state_del it.
 * if no reply comes in time, on_timeout is called instead.
 *
 * every entry carries data_size bytes of your data and is found by its id
 * through a nw_dict. not thread safe, use it from the loop thread.
 * see example/state.c.
 */

typedef struct nw_state_entry {
    /* the timeout timer. must stay the first member */
    ev_timer ev;
    /* the id of the entry, unique within its nw_state, never 0 */
    uint32_t id;
    /* the nw_state the entry belongs to */
    void *context;
    /* your data: data_size bytes, zeroed by nw_state_add. valid until the
     * entry is deleted or timed out */
    void *data;
} nw_state_entry;

typedef struct nw_state_type {
    /* must
     *
     * called when an entry was not deleted within its timeout. when the
     * callback returns, on_release is called and the entry is freed, don't
     * keep it. during the call it is already out of the table:
     * nw_state_get on its id returns NULL, there is no need to delete it,
     * and the same id may be added again */
    void (*on_timeout)(nw_state_entry *entry);
    /* optional
     *
     * called whenever an entry goes away - deleted, timed out, or the whole
     * nw_state released - to free what entry->data points to */
    void (*on_release)(nw_state_entry *entry);
} nw_state_type;

/* the fields are for reading only */
typedef struct nw_state {
    struct ev_loop *loop;
    nw_state_type type;
    uint32_t data_size;
    nw_cache *cache;
    /* id -> nw_state_entry */
    nw_dict *dict;
    uint32_t id_start;
} nw_state;

/* lives on the stack of the caller, see nw_state_iter_init */
typedef struct nw_state_iterator {
    nw_dict_iterator iter;
} nw_state_iterator;

/* create a state table whose entries carry data_size bytes of user data
 * each (0 for none). type is copied. return NULL if on_timeout is not set
 * or memory runs out */
nw_state *nw_state_create(nw_state_type *type, uint32_t data_size);

/* add an entry that times out after `timeout` seconds.
 * id 0: an unused id is picked for you, read it from entry->id.
 * id not 0: use that id; fails if the table already has it.
 * return the new entry with zeroed data, or NULL on failure */
nw_state_entry *nw_state_add(nw_state *state, double timeout, uint32_t id);

/* find an entry by id. return NULL if there is none: never added, deleted,
 * or timed out already */
nw_state_entry *nw_state_get(nw_state *state, uint32_t id);

/* restart the timeout of an entry: it now times out `timeout` seconds from
 * this call. return 0, or -1 if there is no such entry */
int nw_state_mod(nw_state *state, uint32_t id, double timeout);

/* delete an entry: its timeout is cancelled, on_release is called and the
 * entry is freed. return 0, or -1 if there is no such entry */
int nw_state_del(nw_state *state, uint32_t id);

/* number of entries currently in the table */
size_t nw_state_count(nw_state *state);

/* free the table and every entry left in it (on_release is called for
 * each, on_timeout is not) */
void nw_state_release(nw_state *state);

/*
 * walk over all entries, in no particular order:
 *
 *   nw_state_iterator iter;
 *   nw_state_entry *entry;
 *   nw_state_iter_init(&iter, state);
 *   while ((entry = nw_state_next(&iter)) != NULL) { ... }
 *
 * deleting the entry just returned is allowed; adding entries or deleting
 * other ones invalidates the iterator
 */
void nw_state_iter_init(nw_state_iterator *iter, nw_state *state);
/* the next entry, NULL when all were visited */
nw_state_entry *nw_state_next(nw_state_iterator *iter);

# endif
