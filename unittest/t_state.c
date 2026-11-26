/* nw_state: add/get/mod/del, timeout, release hook, growth, iterator */
# include "common.h"

struct data {
    int value;
};

static int timeout_count;
static int release_count;
static int timeout_value_sum;

static void on_timeout(nw_state_entry *entry)
{
    struct data *d = entry->data;
    timeout_count++;
    timeout_value_sum += d->value;
}

static void on_release(nw_state_entry *entry)
{
    release_count++;
}

/* a retry pattern: the timed out state is added again under the same id */
static nw_state *retry_state;
static int retry_timeouts;

static void retry_on_timeout(nw_state_entry *entry)
{
    uint32_t id = entry->id;
    CHECK(nw_state_get(retry_state, id) == NULL);   /* already out of the table */
    CHECK_EQ(nw_state_del(retry_state, id), -1);
    if (++retry_timeouts < 3) {
        nw_state_entry *again = nw_state_add(retry_state, 0.01, id);
        CHECK(again != NULL);
        CHECK(again != entry);
    }
}

int main(void)
{
    nw_state_type type;
    memset(&type, 0, sizeof(type));
    CHECK(nw_state_create(&type, sizeof(struct data)) == NULL);     /* on_timeout is required */
    type.on_timeout = on_timeout;
    type.on_release = on_release;
    nw_state *state = nw_state_create(&type, sizeof(struct data));
    CHECK(state != NULL);

    /* auto id starts at 1, custom id honoured, duplicates rejected */
    nw_state_entry *a = nw_state_add(state, 0.02, 0);
    CHECK(a != NULL);
    CHECK_EQ(a->id, 1);
    ((struct data *)a->data)->value = 10;
    nw_state_entry *b = nw_state_add(state, 0.02, 1000);
    CHECK(b != NULL);
    CHECK_EQ(b->id, 1000);
    ((struct data *)b->data)->value = 20;
    CHECK(nw_state_add(state, 0.02, 1000) == NULL);
    CHECK(nw_state_get(state, 1) == a);
    CHECK(nw_state_get(state, 1000) == b);
    CHECK(nw_state_get(state, 7) == NULL);
    CHECK_EQ(nw_state_count(state), 2);

    /* delete runs the release hook, the timer never fires */
    CHECK_EQ(nw_state_del(state, 1000), 0);
    CHECK_EQ(nw_state_del(state, 1000), -1);
    CHECK_EQ(release_count, 1);
    CHECK_EQ(nw_state_set_timeout(state, 1000, 1.0), -1);

    /* c is pushed far into the future by mod, so only a times out */
    nw_state_entry *c = nw_state_add(state, 0.02, 0);
    CHECK(c != NULL);
    uint32_t c_id = c->id;
    CHECK_EQ(nw_state_set_timeout(state, c_id, 60.0), 0);

    ut_run_for(0.2);
    CHECK_EQ(timeout_count, 1);
    CHECK_EQ(timeout_value_sum, 10);
    CHECK_EQ(release_count, 2);
    CHECK(nw_state_get(state, 1) == NULL);
    CHECK(nw_state_get(state, c_id) == c);
    CHECK_EQ(nw_state_count(state), 1);

    /* grow well past the initial 64 buckets */
    for (int i = 0; i < 2000; ++i) {
        nw_state_entry *e = nw_state_add(state, 60.0, 0);
        CHECK(e != NULL);
        ((struct data *)e->data)->value = i;
    }
    CHECK_EQ(nw_state_count(state), 2001);
    CHECK(state->dict->size > 64);
    CHECK(nw_state_get(state, c_id) == c);

    nw_state_iterator iter;
    nw_state_iter_init(&iter, state);
    int seen = 0;
    nw_state_entry *e;
    while ((e = nw_state_next(&iter)) != NULL) {
        seen++;
        if (e->id % 2 == 0)
            CHECK_EQ(nw_state_del(state, e->id), 0);    /* deleting the current entry is allowed */
    }
    CHECK_EQ(seen, 2001);
    CHECK(nw_state_count(state) < 2001);

    int before = release_count;
    size_t left = nw_state_count(state);
    nw_state_release(state);
    CHECK_EQ(release_count - before, left);

    nw_state_type retry_type;
    memset(&retry_type, 0, sizeof(retry_type));
    retry_type.on_timeout = retry_on_timeout;
    retry_state = nw_state_create(&retry_type, 0);
    CHECK(retry_state != NULL);
    CHECK(nw_state_add(retry_state, 0.01, 42) != NULL);
    ut_run_for(0.2);
    CHECK_EQ(retry_timeouts, 3);
    CHECK_EQ(nw_state_count(retry_state), 0);
    nw_state_release(retry_state);

    return 0;
}
