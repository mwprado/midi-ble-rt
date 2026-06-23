#include "mb-slice-ring.h"

#include <string.h>

const char *mb_slice_ring_kind_name(MbSliceRingKind kind) {
    switch (kind) {
        case MB_SLICE_RING_RX_RAW: return "rx.raw";
        case MB_SLICE_RING_RX_DECODED: return "rx.decoded";
        case MB_SLICE_RING_RX_ACTIVE: return "rx.active";
        case MB_SLICE_RING_TX_RAW: return "tx.raw";
        case MB_SLICE_RING_TX_DECODED: return "tx.decoded";
        case MB_SLICE_RING_TX_ACTIVE: return "tx.active";
        case MB_SLICE_RING_KIND_COUNT:
        default: return "unknown";
    }
}

void mb_slice_ring_init(MbSliceRing *ring,
                        MbSliceRingKind kind,
                        MbFrameModelPool *pool) {
    if (!ring)
        return;

    memset(ring, 0, sizeof(*ring));
    ring->kind = kind;
    ring->pool = pool;
}

void mb_slice_ring_clear(MbSliceRing *ring) {
    if (!ring)
        return;

    while (ring->count > 0)
        mb_slice_ring_drop(ring);

    MbSliceRingKind kind = ring->kind;
    MbFrameModelPool *pool = ring->pool;
    mb_slice_ring_init(ring, kind, pool);
}

static bool push_item(MbSliceRing *ring,
                      MbFrameModelSlice slice,
                      uint64_t timestamp_ns) {
    if (!ring || !ring->pool || !mb_frame_model_slice_ok(slice))
        return false;

    if (ring->count >= MB_SLICE_RING_COUNT) {
        ring->overflow++;
        return false;
    }

    MbSliceRingItem *item = &ring->item[ring->write_idx];
    item->slice = slice;
    item->seq = ring->next_seq++;
    item->timestamp_ns = timestamp_ns;
    item->flags = 0;

    ring->write_idx = (uint8_t)((ring->write_idx + 1u) % MB_SLICE_RING_COUNT);
    ring->count++;
    ring->pushed++;
    return true;
}

bool mb_slice_ring_push_owned(MbSliceRing *ring,
                              MbFrameModelSlice slice,
                              uint64_t timestamp_ns) {
    return push_item(ring, slice, timestamp_ns);
}

bool mb_slice_ring_push_shared(MbSliceRing *ring,
                               MbFrameModelSlice slice,
                               uint64_t timestamp_ns) {
    if (!ring || !ring->pool)
        return false;

    if (!mb_frame_model_hold(ring->pool, slice))
        return false;

    if (!push_item(ring, slice, timestamp_ns)) {
        mb_frame_model_done(ring->pool, slice);
        return false;
    }

    return true;
}

bool mb_slice_ring_peek(const MbSliceRing *ring, const MbSliceRingItem **item) {
    if (!ring || !item || ring->count == 0) {
        if (item)
            *item = NULL;
        return false;
    }

    *item = &ring->item[ring->read_idx];
    return true;
}

bool mb_slice_ring_take(MbSliceRing *ring, MbSliceRingItem *out_item) {
    if (!ring || ring->count == 0)
        return false;

    MbSliceRingItem *item = &ring->item[ring->read_idx];
    if (out_item)
        *out_item = *item;

    memset(item, 0, sizeof(*item));
    ring->read_idx = (uint8_t)((ring->read_idx + 1u) % MB_SLICE_RING_COUNT);
    ring->count--;
    ring->taken++;
    return true;
}

bool mb_slice_ring_drop(MbSliceRing *ring) {
    MbSliceRingItem item;
    if (!mb_slice_ring_take(ring, &item))
        return false;

    mb_slice_ring_item_done(ring, &item);
    ring->dropped++;
    return true;
}

void mb_slice_ring_item_done(MbSliceRing *ring, MbSliceRingItem *item) {
    if (!ring || !ring->pool || !item)
        return;

    mb_frame_model_done(ring->pool, item->slice);
    memset(item, 0, sizeof(*item));
}

bool mb_slice_ring_is_empty(const MbSliceRing *ring) {
    return !ring || ring->count == 0;
}

bool mb_slice_ring_is_full(const MbSliceRing *ring) {
    return ring && ring->count >= MB_SLICE_RING_COUNT;
}

uint8_t mb_slice_ring_count(const MbSliceRing *ring) {
    return ring ? ring->count : 0;
}

uint64_t mb_slice_ring_overflow_count(const MbSliceRing *ring) {
    return ring ? ring->overflow : 0;
}
