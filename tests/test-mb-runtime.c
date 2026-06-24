#include "mb-runtime.h"

#include <glib.h>
#include <string.h>

typedef struct {
    GMutex lock;
    GCond cond;
    unsigned expected;
    unsigned consumed;
    uint8_t checksum;
} RuntimeTestState;

static void runtime_test_state_init(RuntimeTestState *state, unsigned expected) {
    memset(state, 0, sizeof(*state));
    g_mutex_init(&state->lock);
    g_cond_init(&state->cond);
    state->expected = expected;
}

static void runtime_test_state_clear(RuntimeTestState *state) {
    g_cond_clear(&state->cond);
    g_mutex_clear(&state->lock);
}

static void consume_packet(MbRuntimeFlow *flow,
                           const MbSliceRingItem *item,
                           const uint8_t *data,
                           size_t len,
                           void *user_data) {
    (void)flow;
    (void)item;

    RuntimeTestState *state = user_data;
    g_mutex_lock(&state->lock);
    for (size_t i = 0; i < len; i++)
        state->checksum ^= data[i];
    state->consumed++;
    if (state->consumed >= state->expected)
        g_cond_signal(&state->cond);
    g_mutex_unlock(&state->lock);
}

static bool wait_consumed(RuntimeTestState *state, gint64 timeout_us) {
    gint64 deadline = g_get_monotonic_time() + timeout_us;
    bool ok = true;

    g_mutex_lock(&state->lock);
    while (state->consumed < state->expected) {
        if (!g_cond_wait_until(&state->cond, &state->lock, deadline)) {
            ok = false;
            break;
        }
    }
    g_mutex_unlock(&state->lock);
    return ok;
}

static void test_runtime_flow_consumes_async(void) {
    RuntimeTestState state;
    runtime_test_state_init(&state, 3);

    MbRuntimeFlow flow;
    mb_runtime_flow_init(&flow,
                         "test-rx-flow",
                         MB_SLICE_RING_RX_RAW,
                         consume_packet,
                         &state);

    g_assert_true(mb_runtime_flow_start(&flow));

    uint8_t a[3] = {0x90, 0x40, 0x7f};
    uint8_t b[3] = {0x80, 0x40, 0x00};
    uint8_t c[1] = {0xfe};

    g_assert_true(mb_runtime_flow_push_bytes(&flow, a, sizeof(a), 1));
    g_assert_true(mb_runtime_flow_push_bytes(&flow, b, sizeof(b), 2));
    g_assert_true(mb_runtime_flow_push_bytes(&flow, c, sizeof(c), 3));

    g_assert_true(wait_consumed(&state, 2 * G_USEC_PER_SEC));
    g_assert_cmpuint(flow.consumed, ==, 3);
    g_assert_cmpuint(mb_runtime_flow_depth(&flow), ==, 0);

    mb_runtime_flow_clear(&flow);
    runtime_test_state_clear(&state);
}

static void test_runtime_flow_rejects_oversize(void) {
    RuntimeTestState state;
    runtime_test_state_init(&state, 0);

    MbRuntimeFlow flow;
    mb_runtime_flow_init(&flow,
                         "test-tx-flow",
                         MB_SLICE_RING_TX_RAW,
                         consume_packet,
                         &state);

    uint8_t too_big[MB_FRAME_MODEL_CAPACITY + 1];
    memset(too_big, 0x55, sizeof(too_big));

    g_assert_false(mb_runtime_flow_push_bytes(&flow, too_big, sizeof(too_big), 1));
    g_assert_cmpuint(flow.push_failures, ==, 1);

    mb_runtime_flow_clear(&flow);
    runtime_test_state_clear(&state);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/mb-runtime/flow-consumes-async", test_runtime_flow_consumes_async);
    g_test_add_func("/mb-runtime/rejects-oversize", test_runtime_flow_rejects_oversize);

    return g_test_run();
}
