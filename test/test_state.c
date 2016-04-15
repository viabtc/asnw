/*
 * Description: 
 *     History: yang@haipo.me, 2016/03/23, create
 */

# include <stdio.h>
# include <errno.h>
# include <error.h>

# include "nw_state.h"

struct test_data {
    char *str;
};

void on_timeout(nw_state_entry *entry)
{
    struct test_data *data = entry->data;
    printf("on_timeout %s\n", data->str);
}

void on_release(nw_state_entry *entry)
{
    struct test_data *data = entry->data;
    printf("on_release %s\n", data->str);
    free(data->str);
}

int main(int argc, char *argv[])
{
    struct nw_state_type type;
    type.on_timeout = on_timeout;
    type.on_release = on_release;

    nw_state *state = nw_state_create(&type, sizeof(struct test_data));
    if (state == NULL) {
        error(1, errno, "nw_state_create fail");
    }

    nw_state_entry *entry = nw_state_add(state, 1.0);
    if (entry == NULL) {
        error(1, errno, "nw_state_add fail");
    }
    printf("id: %d\n", entry->id);

    struct test_data *data = entry->data;
    data->str = strdup("hello world");

    entry = nw_state_get(state, entry->id);
    if (!entry) {
        error(1, errno, "nw_state_get fail");
    }
    if (nw_state_mod(state, entry->id, 5.0) < 0) {
        error(1, errno, "nw_state_mod fail");
    }
    nw_state_del(state, 1);

    for (int i = 0; i < 100; ++i) {
        entry = nw_state_add(state, 1.0);
        if (entry == NULL) {
            error(1, errno, "nw_state_add fail");
        }

        data = entry->data;
        data->str = strdup("asdfghjkl");
    }

    printf("state count: %zu\n", nw_state_count(state));
    printf("state table_size: %u\n", state->table_size);

    nw_state_iterator *iter = nw_state_get_iterator(state);
    if (iter == NULL) {
        error(1, errno, "nw_state_get_iterator fail");
    }
    while ((entry = nw_state_next(iter)) != NULL) {
        data = entry->data;
        printf("%u: %s\n", entry->id, data->str);
    }
    nw_state_iterator_release(iter);

    nw_loop_run();

    return 0;
}

