#include "mb-duplex-runtime.h"

#include <glib.h>
#include <string.h>

typedef struct {
    GMutex lock;
    GCond cond;
    unsigned rx_expected;
    unsigned tx_expected;
    unsigned rx_seen;
    unsigned tx_seen;
} DuplexState;

static void duplex_state_init(DuplexState *state, unsigned rx_expected, unsigned tx_expected) {
    memset(state, 0, sizeof(*state));
    g_mutex_init(&state->lock);
    g_cond_init(&state->cond);
    state->rx_expected = rx_expected;
    state->tx_expected = tx_expected;
}

static void duplex_state_clear(DuplexState *state) {
    g_cond_clear(&state->cond);
    g_mutex_clear(&state->lock);
}

static void rx_consume(MbRuntimeFlow *flow,
                       const MbSliceRingItem *item,
                       const uint8_t *data,
                       size_t len,
                       void *user_data) {
    (void)flow;
    (void)item;
    g_assert_nonnull(data);
    g_assert_cmpuint(len, >, 0);

    DuplexState *state = user_data;
    g_mutex_lock(&state->lock);
    state->rx_seen++;
    if (state->rx_seen >= state->rx_expected && state->tx_seen >= state->tx_expected)
        g_cond_signal(&state->cond);
    g_mutex_unlock(&state->lock);
}

static void tx_consume(MbRuntimeFlow *flow,
                       const MbSliceRingItem *item,
                       const uint8_t *data,
                       size_t len,
                       void *user_data) {
    (void)flow;
    (void)item;
    g_assert_nonnull(data);
    g_assert_cmpuint(len, >, 0);

    DuplexState *state = user_data;
    g_mutex_lock(&state->lock);
    state->tx_seen++;
    if (state->rx_seen >= state->rx_expected && state->tx_seen >= state->tx_expected)
        g_cond_signal(&state->cond);
    g_mutex_unlock(&state->lock);
}

static bool wait_duplex(DuplexState *state, gint64 timeout_us) {
    gint64 deadline = g_get_monotonic_time() + timeout_us;
    bool ok = true;

    g_mutex_lock(&state->lock);
    while (state->rx_seen < state->rx_expected || state->tx_seen < state->tx_expected) {
        if (!g_cond_wait_until(&state->cond, &state->lock, deadline)) {
            ok = false;
            break;
        }
    }
    g_mutex_unlock(&state->lock);
    return ok;
}

static void test_duplex_runtime_rx_tx_independent(void) {
    DuplexState state;
    duplex_state_init(&state, 2, 2);

    MbDuplexRuntime runtime;
    mb_duplex_runtime_init(&runtime, rx_consume, &state, tx_consume, &state);
    g_assert_true(mb_duplex_runtime_start(&runtime));

    uint8_t rx1[] = {0x90, 0x40, 0x7f};
    uint8_t rx2[] = {0x80, 0x40, 0x00};
    uint8_t tx1[] = {0xb0, 0x07, 0x64};
    uint8_t tx2[] = {0xc0, 0x05};

    g_assert_true(mb_duplex_runtime_push_rx(&runtime, rx1, sizeof(rx1), 1));
    g_assert_true(mb_duplex_runtime_push_tx(&runtime, tx1, sizeof(tx1), 2));
    g_assert_true(mb_duplex_runtime_push_rx(&runtime, rx2, sizeof(rx2), 3));
    g_assert_true(mb_duplex_runtime_push_tx(&runtime, tx2, sizeof(tx2), 4));

    g_assert_true(wait_duplex(&state, 2 * G_USEC_PER_SEC));
    g_assert_cmpuint(state.rx_seen, ==, 2);
    g_assert_cmpuint(state.tx_seen, ==, 2);
    g_assert_cmpuint(mb_duplex_runtime_rx_depth(&runtime), ==, 0);
    g_assert_cmpuint(mb_duplex_runtime_tx_depth(&runtime), ==, 0);

    mb_duplex_runtime_clear(&runtime);
    duplex_state_clear(&state);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/mb-duplex-runtime/rx-tx-independent", test_duplex_runtime_rx_tx_independent);

    return g_test_run();
}
