#include "mb-buffer.h"

#include <string.h>

static uint16_t clamp_limit(uint16_t limit) {
    if (limit < MB_BUFFER_LIMIT_MIN)
        return MB_BUFFER_LIMIT_MIN;
    if (limit > MB_BUFFER_LIMIT_MAX)
        return MB_BUFFER_LIMIT_MAX;
    return limit;
}

const char *mb_buffer_kind_name(MbBufferKind kind) {
    switch (kind) {
        case MB_BUFFER_RX_RAW: return "rx.raw";
        case MB_BUFFER_RX_DECODED: return "rx.decoded";
        case MB_BUFFER_RX_ACTIVE: return "rx.active";
        case MB_BUFFER_TX_RAW: return "tx.raw";
        case MB_BUFFER_TX_DECODED: return "tx.decoded";
        case MB_BUFFER_TX_ACTIVE: return "tx.active";
        case MB_BUFFER_KIND_COUNT:
        default: return "unknown";
    }
}

void mb_buffer_ring_init(MbBufferRing *ring, MbBufferKind kind, uint64_t now_ns) {
    if (!ring)
        return;

    memset(ring, 0, sizeof(*ring));
    ring->kind = kind;
    ring->active_limit = MB_BUFFER_LIMIT_MIN;
    ring->last_pressure_ns = now_ns;
}

void mb_buffer_ring_reset(MbBufferRing *ring, uint64_t now_ns) {
    if (!ring)
        return;

    MbBufferKind kind = ring->kind;
    mb_buffer_ring_init(ring, kind, now_ns);
}

static void mark_pressure(MbBufferRing *ring, uint64_t now_ns) {
    ring->last_pressure_ns = now_ns;
}

static void maybe_grow_for_len(MbBufferRing *ring, size_t len, uint64_t now_ns) {
    while (len > ring->active_limit && ring->active_limit < MB_BUFFER_LIMIT_MAX) {
        ring->active_limit = clamp_limit((uint16_t)(ring->active_limit * 2u));
        ring->grow_count++;
        mark_pressure(ring, now_ns);
    }
}

bool mb_buffer_ring_push(MbBufferRing *ring,
                         const uint8_t *data,
                         size_t len,
                         uint64_t timestamp_ns,
                         uint64_t now_ns) {
    if (!ring)
        return false;
    if (len > 0 && !data)
        return false;

    if (len > MB_BUFFER_SLOT_CAPACITY) {
        ring->oversize_drops++;
        ring->overflows++;
        mark_pressure(ring, now_ns);
        return false;
    }

    maybe_grow_for_len(ring, len, now_ns);

    if (ring->count >= MB_BUFFER_SLOT_COUNT) {
        ring->overflows++;
        mark_pressure(ring, now_ns);
        return false;
    }

    MbBufferSlot *slot = &ring->slots[ring->write_idx];
    if (len > 0)
        memcpy(slot->data, data, len);
    slot->len = (uint16_t)len;
    slot->seq = ring->next_seq++;
    slot->timestamp_ns = timestamp_ns;
    slot->flags = 0;

    ring->write_idx = (uint8_t)((ring->write_idx + 1u) % MB_BUFFER_SLOT_COUNT);
    ring->count++;
    ring->pushed++;

    if (slot->len > ring->max_slot_len)
        ring->max_slot_len = slot->len;
    if (ring->count > ring->max_depth)
        ring->max_depth = ring->count;

    if (slot->len > MB_BUFFER_SHRINK_THRESHOLD || ring->count > MB_BUFFER_DEPTH_PRESSURE)
        mark_pressure(ring, now_ns);

    return true;
}

bool mb_buffer_ring_peek(const MbBufferRing *ring, const MbBufferSlot **slot) {
    if (!ring || !slot || ring->count == 0) {
        if (slot)
            *slot = NULL;
        return false;
    }

    *slot = &ring->slots[ring->read_idx];
    return true;
}

bool mb_buffer_ring_pop(MbBufferRing *ring, MbBufferSlot *out_slot) {
    if (!ring || ring->count == 0)
        return false;

    MbBufferSlot *slot = &ring->slots[ring->read_idx];
    if (out_slot)
        *out_slot = *slot;

    memset(slot, 0, sizeof(*slot));
    ring->read_idx = (uint8_t)((ring->read_idx + 1u) % MB_BUFFER_SLOT_COUNT);
    ring->count--;
    ring->popped++;
    return true;
}

void mb_buffer_ring_tick(MbBufferRing *ring, uint64_t now_ns) {
    if (!ring)
        return;

    if (ring->active_limit <= MB_BUFFER_LIMIT_MIN)
        return;

    if (now_ns < ring->last_pressure_ns)
        return;

    if (now_ns - ring->last_pressure_ns < MB_BUFFER_SHRINK_AFTER_NS)
        return;

    ring->active_limit = clamp_limit((uint16_t)(ring->active_limit / 2u));
    ring->shrink_count++;
    ring->last_pressure_ns = now_ns;

    /* Window metrics are reset after a shrink step. Cumulative counters remain. */
    ring->max_slot_len = 0;
    ring->max_depth = ring->count;
}

bool mb_buffer_ring_is_empty(const MbBufferRing *ring) {
    return !ring || ring->count == 0;
}

bool mb_buffer_ring_is_full(const MbBufferRing *ring) {
    return ring && ring->count >= MB_BUFFER_SLOT_COUNT;
}

uint16_t mb_buffer_ring_active_limit(const MbBufferRing *ring) {
    return ring ? ring->active_limit : 0;
}

uint8_t mb_buffer_ring_count(const MbBufferRing *ring) {
    return ring ? ring->count : 0;
}

uint64_t mb_buffer_ring_overflows(const MbBufferRing *ring) {
    return ring ? ring->overflows : 0;
}

void mb_buffer_pipeline_init(MbBufferPipeline *pipeline,
                             MbBufferKind raw_kind,
                             MbBufferKind decoded_kind,
                             MbBufferKind active_kind,
                             uint64_t now_ns) {
    if (!pipeline)
        return;

    mb_buffer_ring_init(&pipeline->raw, raw_kind, now_ns);
    mb_buffer_ring_init(&pipeline->decoded, decoded_kind, now_ns);
    mb_buffer_ring_init(&pipeline->active, active_kind, now_ns);
}

void mb_buffer_pipeline_reset(MbBufferPipeline *pipeline,
                              MbBufferKind raw_kind,
                              MbBufferKind decoded_kind,
                              MbBufferKind active_kind,
                              uint64_t now_ns) {
    mb_buffer_pipeline_init(pipeline, raw_kind, decoded_kind, active_kind, now_ns);
}

void mb_buffer_pipeline_tick(MbBufferPipeline *pipeline, uint64_t now_ns) {
    if (!pipeline)
        return;

    mb_buffer_ring_tick(&pipeline->raw, now_ns);
    mb_buffer_ring_tick(&pipeline->decoded, now_ns);
    mb_buffer_ring_tick(&pipeline->active, now_ns);
}

uint64_t mb_buffer_pipeline_total_overflows(const MbBufferPipeline *pipeline) {
    if (!pipeline)
        return 0;

    return pipeline->raw.overflows +
           pipeline->decoded.overflows +
           pipeline->active.overflows;
}

uint16_t mb_buffer_pipeline_max_active_limit(const MbBufferPipeline *pipeline) {
    if (!pipeline)
        return 0;

    uint16_t max = pipeline->raw.active_limit;
    if (pipeline->decoded.active_limit > max)
        max = pipeline->decoded.active_limit;
    if (pipeline->active.active_limit > max)
        max = pipeline->active.active_limit;
    return max;
}

void mb_session_buffers_init(MbSessionBuffers *buffers, uint64_t now_ns) {
    if (!buffers)
        return;

    mb_buffer_pipeline_init(&buffers->rx,
                            MB_BUFFER_RX_RAW,
                            MB_BUFFER_RX_DECODED,
                            MB_BUFFER_RX_ACTIVE,
                            now_ns);
    mb_buffer_pipeline_init(&buffers->tx,
                            MB_BUFFER_TX_RAW,
                            MB_BUFFER_TX_DECODED,
                            MB_BUFFER_TX_ACTIVE,
                            now_ns);
}

void mb_session_buffers_reset(MbSessionBuffers *buffers, uint64_t now_ns) {
    mb_session_buffers_init(buffers, now_ns);
}

void mb_session_buffers_tick(MbSessionBuffers *buffers, uint64_t now_ns) {
    if (!buffers)
        return;

    mb_buffer_pipeline_tick(&buffers->rx, now_ns);
    mb_buffer_pipeline_tick(&buffers->tx, now_ns);
}

uint64_t mb_session_buffers_total_overflows(const MbSessionBuffers *buffers) {
    if (!buffers)
        return 0;

    return mb_buffer_pipeline_total_overflows(&buffers->rx) +
           mb_buffer_pipeline_total_overflows(&buffers->tx);
}

uint16_t mb_session_buffers_max_active_limit(const MbSessionBuffers *buffers) {
    if (!buffers)
        return 0;

    uint16_t rx_max = mb_buffer_pipeline_max_active_limit(&buffers->rx);
    uint16_t tx_max = mb_buffer_pipeline_max_active_limit(&buffers->tx);
    return rx_max > tx_max ? rx_max : tx_max;
}
