#include "mb-runtime.h"

#include <string.h>

static gpointer runtime_flow_thread(gpointer data) {
    MbRuntimeFlow *flow = data;

    for (;;) {
        g_mutex_lock(&flow->wake_lock);
        while (!flow->stop_requested && mb_slice_ring_is_empty(&flow->ring))
            g_cond_wait(&flow->wake_cond, &flow->wake_lock);
        bool stop = flow->stop_requested;
        g_mutex_unlock(&flow->wake_lock);

        if (stop && mb_slice_ring_is_empty(&flow->ring))
            break;

        for (;;) {
            MbSliceRingItem item;
            if (!mb_slice_ring_take(&flow->ring, &item))
                break;

            size_t len = 0;
            const uint8_t *bytes = mb_frame_model_get(&flow->pool, item.slice, &len);
            if (bytes && flow->consume)
                flow->consume(flow, &item, bytes, len, flow->user_data);

            flow->consumed++;
            mb_slice_ring_item_done(&flow->ring, &item);
        }
    }

    return NULL;
}

void mb_runtime_flow_init(MbRuntimeFlow *flow,
                          const char *name,
                          MbSliceRingKind ring_kind,
                          MbRuntimeConsumeFunc consume,
                          void *user_data) {
    if (!flow)
        return;

    memset(flow, 0, sizeof(*flow));
    flow->name = name ? name : "flow";
    flow->consume = consume;
    flow->user_data = user_data;

    mb_frame_model_pool_init(&flow->pool);
    mb_slice_ring_init(&flow->ring, ring_kind, &flow->pool);

    g_mutex_init(&flow->wake_lock);
    g_cond_init(&flow->wake_cond);
    flow->wake_initialized = true;
}

void mb_runtime_flow_clear(MbRuntimeFlow *flow) {
    if (!flow)
        return;

    mb_runtime_flow_stop(flow);
    mb_slice_ring_clear(&flow->ring);
    mb_frame_model_pool_clear(&flow->pool);

    if (flow->wake_initialized) {
        g_cond_clear(&flow->wake_cond);
        g_mutex_clear(&flow->wake_lock);
    }

    memset(flow, 0, sizeof(*flow));
}

bool mb_runtime_flow_start(MbRuntimeFlow *flow) {
    if (!flow || flow->consumer_thread)
        return false;

    flow->stop_requested = false;
    flow->consumer_thread = g_thread_new(flow->name, runtime_flow_thread, flow);
    return flow->consumer_thread != NULL;
}

void mb_runtime_flow_stop(MbRuntimeFlow *flow) {
    if (!flow || !flow->consumer_thread)
        return;

    g_mutex_lock(&flow->wake_lock);
    flow->stop_requested = true;
    g_cond_signal(&flow->wake_cond);
    g_mutex_unlock(&flow->wake_lock);

    g_thread_join(flow->consumer_thread);
    flow->consumer_thread = NULL;
}

void mb_runtime_flow_wake(MbRuntimeFlow *flow) {
    if (!flow || !flow->wake_initialized)
        return;

    g_mutex_lock(&flow->wake_lock);
    g_cond_signal(&flow->wake_cond);
    g_mutex_unlock(&flow->wake_lock);
}

bool mb_runtime_flow_push_bytes(MbRuntimeFlow *flow,
                                const uint8_t *data,
                                size_t len,
                                uint64_t timestamp_ns) {
    if (!flow)
        return false;

    MbFrameModelSlice slice;
    if (!mb_frame_model_make(&flow->pool, data, len, timestamp_ns, &slice)) {
        flow->push_failures++;
        return false;
    }

    if (!mb_slice_ring_push_owned(&flow->ring, slice, timestamp_ns)) {
        mb_frame_model_done(&flow->pool, slice);
        flow->push_failures++;
        return false;
    }

    flow->pushed++;
    mb_runtime_flow_wake(flow);
    return true;
}

uint8_t mb_runtime_flow_depth(const MbRuntimeFlow *flow) {
    return flow ? mb_slice_ring_count(&flow->ring) : 0;
}

uint64_t mb_runtime_flow_overflows(const MbRuntimeFlow *flow) {
    return flow ? mb_slice_ring_overflow_count(&flow->ring) : 0;
}
