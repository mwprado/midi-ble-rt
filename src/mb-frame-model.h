#ifndef MB_FRAME_MODEL_H
#define MB_FRAME_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MB_FRAME_MODEL_COUNT 32u
#define MB_FRAME_MODEL_CAPACITY 256u
#define MB_FRAME_MODEL_INVALID UINT16_MAX

typedef struct {
    uint16_t id;
    uint16_t off;
    uint16_t len;
} MbFrameModelSlice;

typedef struct {
    uint8_t payload[MB_FRAME_MODEL_CAPACITY];
    uint16_t len;
    int busy;
    int holders;
    uint64_t seq;
    uint64_t timestamp_ns;
} MbFrameModelFrame;

typedef struct {
    MbFrameModelFrame frame[MB_FRAME_MODEL_COUNT];
    uint16_t next;
    uint16_t occupied;
    uint16_t occupied_max;
    uint64_t seq_next;
    uint64_t alloc_count;
    uint64_t empty_count;
} MbFrameModelPool;

void mb_frame_model_pool_init(MbFrameModelPool *pool);
void mb_frame_model_pool_clear(MbFrameModelPool *pool);

bool mb_frame_model_make(MbFrameModelPool *pool,
                         const uint8_t *data,
                         size_t len,
                         uint64_t timestamp_ns,
                         MbFrameModelSlice *out_slice);
bool mb_frame_model_hold(MbFrameModelPool *pool, MbFrameModelSlice slice);
bool mb_frame_model_done(MbFrameModelPool *pool, MbFrameModelSlice slice);

const uint8_t *mb_frame_model_get(const MbFrameModelPool *pool,
                                  MbFrameModelSlice slice,
                                  size_t *out_len);
bool mb_frame_model_slice_ok(MbFrameModelSlice slice);

#endif
