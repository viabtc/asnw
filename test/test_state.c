/*
 * Description: 
 *     History: yang@haipo.me, 2016/03/23, create
 */

# include <stdio.h>
# include <errno.h>
# include <error.h>

# include "nw_state.h"

void on_timeout(nw_state_entry *entry)
{
    printf("%s\n", entry->data);
}

int main(int argc, char *argv[])
{
    nw_state *state = nw_state_create(on_timeout);
    if (state == NULL) {
        error(1, errno, "nw_state_create fail");
    }

    char *s = "hello world";
    nw_state_entry *entry = nw_state_add(state, strlen(s) + 1, 1.0);
    if (entry == NULL) {
        error(1, errno, "nw_state_add fail");
    }
    printf("id: %d, size: %u\n", entry->id, entry->data_size);
    strcpy(entry->data, s);

    entry = nw_state_get(state, entry->id);
    if (!entry) {
        error(1, errno, "nw_state_get fail");
    }
    if (nw_state_mod(state, entry->id, 5.0) < 0) {
        error(1, errno, "nw_state_mod fail");
    }
    nw_state_del(state, 1);

    for (int i = 0; i < 100; ++i) {
        s = "abcdabcdabcdabcdabcdabcd1234567890";
        entry = nw_state_add(state, strlen(s) + 1, 1.0);
        if (entry == NULL) {
            error(1, errno, "nw_state_add fail");
        }
        strcpy(entry->data, s);
    }
    printf("state count: %zu\n", nw_state_count(state));
    printf("state table_size: %u\n", state->table_size);

    nw_state_iterator *iter = nw_state_get_iterator(state);
    if (iter == NULL) {
        error(1, errno, "nw_state_get_iterator fail");
    }
    while ((entry = nw_state_next(iter)) != NULL) {
        printf("%u: %s\n", entry->id, entry->data);
    }
    nw_state_iterator_release(iter);

    nw_loop_run();

    return 0;
}

