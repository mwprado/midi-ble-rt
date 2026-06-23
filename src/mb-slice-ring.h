#ifndef MB_SLICE_RING_H
#define MB_SLICE_RING_H

#include "mb-frame-model.h"

#include <stdbool.h>
#include <stdint.h>

#define MB_SLICE_RING_COUNT 16u

typedef enum {
    MB_SLICE_RING_RX_RAW = 0,
    MB_SLICE_RING_RX_DECODED,
    MB_SLICE_RING_RX_ACTIVE,
    MB_SLICE_RING_TX_RAW,
    MB_SLICE_RING_TX_DECODED,
    MB_SLICE_RING_TX_ACTIVE,
    MB_SLICE_RING_KIND_COUNT
} MbSliceRingKind;

typedef struct {
    MbFrameModelSlice slice;
    uint64_t seq;
    uint64_t timestamp_ns;
    uint32_t flags;
} MbSliceRingItem;

typedef struct {
    MbSliceRingKind kind;
    MbFrameModelPool *pool;
    MbSliceRingItem item[MB_SLICE_RING_COUNT];
    uint8_t read_idx;
    uint8_t write_idx;
    uint8_t count;
    uint64_t next_seq;
    uint64_t pushed;
    uint64_t taken;
    uint64_t dropped;
    uint64_t overflow;
} MbSliceRing;

const char *mb_slice_ring_kind_name(MbSliceRingKind kind);

void mb_slice_ring_init(MbSliceRing *ring,
                        MbSliceRingKind kind,
                        MbFrameModelPool *pool);
void mb_slice_ring_clear(MbSliceRing *ring);

bool mb_slice_ring_push_owned(MbSliceRing *ring,
                              MbFrameModelSlice slice,
                              uint64_t timestamp_ns);
bool mb_slice_ring_push_shared(MbSliceRing *ring,
                               MbFrameModelSlice slice,
                               uint64_t timestamp_ns);
bool mb_slice_ring_peek(const MbSliceRing *ring, const MbSliceRingItem **item);
bool mb_slice_ring_take(MbSliceRing *ring, MbSliceRingItem *out_item);
bool mb_slice_ring_drop(MbSliceRing *ring);
void mb_slice_ring_item_done(MbSliceRing *ring, MbSliceRingItem *item);

bool mb_slice_ring_is_empty(const MbSliceRing *ring);
bool mb_slice_ring_is_full(const MbSliceRing *ring);
uint8_t mb_slice_ring_count(const MbSliceRing *ring);
uint64_t mb_slice_ring_overflow_count(const MbSliceRing *ring);

#endif
