#include "mb-runtime.h"

#include "mb-latency-diagnostics.h"

#include <string.h>

static uint64_t mb_runtime_now_ns(void) {
    return (uint64_t)g_get_monotonic_time() * 1000ULL;
}

static MbLatencyDirection mb_runtime_flow_latency_direction(const MbRuntimeFlow *flow) {
    if (flow &&
        (flow->ring.kind == MB_SLICE_RING_TX_RAW ||
         flow->ring.kind == MB_SLICE_RING_TX_DECODED ||
         flow->ring.kind == MB_SLICE_RING_TX_ACTIVE))
        return MB_LATENCY_TX;

    return MB_LATENCY_RX;
}

static void mb_runtime_flow_notify_depth(MbRuntimeFlow *flow) {
    if (!flow || !flow->observe_depth)
        return;

    flow->observe_depth(flow,
                        mb_runtime_flow_depth(flow),
                        flow->observe_depth_user_data);
}

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

            mb_runtime_flow_notify_depth(flow);

            size_t len = 0;
            const uint8_t *bytes = mb_frame_model_get(&flow->pool, item.slice, &len);
            uint64_t consume_started_ns = mb_runtime_now_ns();
            if (bytes && flow->consume)
                flow->consume(flow, &item, bytes, len, flow->user_data);
            uint64_t consume_finished_ns = mb_runtime_now_ns();

            mb_latency_diagnostics_record(mb_runtime_flow_latency_direction(flow),
                                          item.timestamp_ns,
                                          consume_started_ns,
                                          consume_finished_ns);

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

void mb_runtime_flow_set_depth_observer(MbRuntimeFlow *flow,
                                        MbRuntimeDepthObserveFunc observe_depth,
                                        void *user_data) {
    if (!flow)
        return;

    flow->observe_depth = observe_depth;
    flow->observe_depth_user_data = user_data;
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
    return mb_runtime_flow_push_bytes_with_epoch(flow, data, len, timestamp_ns, 0);
}

bool mb_runtime_flow_push_bytes_with_epoch(MbRuntimeFlow *flow,
                                            const uint8_t *data,
                                            size_t len,
                                            uint64_t timestamp_ns,
                                            uint64_t epoch) {
    if (!flow)
        return false;

    if (len > MB_FRAME_MODEL_CAPACITY) {
        g_printerr("%s runtime frame too large: len=%zu cap=%u; dropping.\n",
                   flow->name ? flow->name : "flow",
                   len,
                   (unsigned)MB_FRAME_MODEL_CAPACITY);
        flow->push_failures++;
        flow->dropped++;
        return false;
    }

    MbFrameModelSlice slice;
    if (!mb_frame_model_make(&flow->pool, data, len, timestamp_ns, &slice)) {
        flow->push_failures++;
        return false;
    }

    if (!mb_slice_ring_push_owned_with_epoch(&flow->ring, slice, timestamp_ns, epoch)) {
        mb_frame_model_done(&flow->pool, slice);
        flow->push_failures++;
        return false;
    }

    flow->pushed++;
    mb_runtime_flow_notify_depth(flow);
    mb_runtime_flow_wake(flow);
    return true;
}

void mb_runtime_flow_drop_pending(MbRuntimeFlow *flow) {
    if (!flow)
        return;

    while (mb_slice_ring_drop(&flow->ring))
        flow->dropped++;

    mb_runtime_flow_notify_depth(flow);
}

uint8_t mb_runtime_flow_depth(const MbRuntimeFlow *flow) {
    return flow ? mb_slice_ring_count(&flow->ring) : 0;
}

uint64_t mb_runtime_flow_overflows(const MbRuntimeFlow *flow) {
    return flow ? mb_slice_ring_overflow_count(&flow->ring) : 0;
}

MbRuntimeFlowStats mb_runtime_flow_stats(const MbRuntimeFlow *flow) {
    MbRuntimeFlowStats stats = {0};
    if (!flow)
        return stats;

    stats.pushed = flow->pushed;
    stats.consumed = flow->consumed;
    stats.dropped = flow->dropped;
    stats.push_failures = flow->push_failures;
    stats.overflows = mb_runtime_flow_overflows(flow);
    stats.depth = mb_runtime_flow_depth(flow);
    return stats;
}
