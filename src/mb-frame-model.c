#include "mb-frame-model.h"

#include <glib.h>
#include <string.h>

void mb_frame_model_pool_init(MbFrameModelPool *pool) {
    if (!pool)
        return;

    memset(pool, 0, sizeof(*pool));
}

void mb_frame_model_pool_clear(MbFrameModelPool *pool) {
    mb_frame_model_pool_init(pool);
}

bool mb_frame_model_slice_ok(MbFrameModelSlice slice) {
    return slice.id != MB_FRAME_MODEL_INVALID &&
           slice.id < MB_FRAME_MODEL_COUNT &&
           slice.off <= MB_FRAME_MODEL_CAPACITY &&
           slice.len <= MB_FRAME_MODEL_CAPACITY &&
           (uint32_t)slice.off + (uint32_t)slice.len <= MB_FRAME_MODEL_CAPACITY;
}

static bool frame_slot_claim(MbFrameModelFrame *frame) {
    return g_atomic_int_compare_and_exchange(&frame->busy, 0, 1);
}

bool mb_frame_model_make(MbFrameModelPool *pool,
                         const uint8_t *data,
                         size_t len,
                         uint64_t timestamp_ns,
                         MbFrameModelSlice *out_slice) {
    if (out_slice)
        *out_slice = (MbFrameModelSlice){MB_FRAME_MODEL_INVALID, 0, 0};

    if (!pool || len > MB_FRAME_MODEL_CAPACITY)
        return false;
    if (len > 0 && !data)
        return false;

    for (uint16_t step = 0; step < MB_FRAME_MODEL_COUNT; step++) {
        uint16_t id = (uint16_t)((pool->next + step) % MB_FRAME_MODEL_COUNT);
        MbFrameModelFrame *frame = &pool->frame[id];

        if (!frame_slot_claim(frame))
            continue;

        if (len > 0)
            memcpy(frame->payload, data, len);
        frame->len = (uint16_t)len;
        frame->holders = 1;
        frame->seq = pool->seq_next++;
        frame->timestamp_ns = timestamp_ns;

        pool->next = (uint16_t)((id + 1u) % MB_FRAME_MODEL_COUNT);
        pool->alloc_count++;
        if (pool->occupied < MB_FRAME_MODEL_COUNT)
            pool->occupied++;
        if (pool->occupied > pool->occupied_max)
            pool->occupied_max = pool->occupied;

        if (out_slice)
            *out_slice = (MbFrameModelSlice){id, 0, (uint16_t)len};
        return true;
    }

    pool->empty_count++;
    return false;
}

bool mb_frame_model_hold(MbFrameModelPool *pool, MbFrameModelSlice slice) {
    if (!pool || !mb_frame_model_slice_ok(slice))
        return false;

    MbFrameModelFrame *frame = &pool->frame[slice.id];
    if (!g_atomic_int_get(&frame->busy))
        return false;
    if (slice.off + slice.len > frame->len)
        return false;

    g_atomic_int_inc(&frame->holders);
    return true;
}

bool mb_frame_model_done(MbFrameModelPool *pool, MbFrameModelSlice slice) {
    if (!pool || !mb_frame_model_slice_ok(slice))
        return false;

    MbFrameModelFrame *frame = &pool->frame[slice.id];
    if (!g_atomic_int_get(&frame->busy))
        return false;

    if (g_atomic_int_dec_and_test(&frame->holders)) {
        frame->len = 0;
        frame->seq = 0;
        frame->timestamp_ns = 0;
        g_atomic_int_set(&frame->busy, 0);
        if (pool->occupied > 0)
            pool->occupied--;
    }

    return true;
}

const uint8_t *mb_frame_model_get(const MbFrameModelPool *pool,
                                  MbFrameModelSlice slice,
                                  size_t *out_len) {
    if (out_len)
        *out_len = 0;

    if (!pool || !mb_frame_model_slice_ok(slice))
        return NULL;

    const MbFrameModelFrame *frame = &pool->frame[slice.id];
    if (!g_atomic_int_get((gint *)&frame->busy))
        return NULL;
    if (slice.off + slice.len > frame->len)
        return NULL;

    if (out_len)
        *out_len = slice.len;
    return &frame->payload[slice.off];
}
