#ifndef MB_FRAME_MODEL_H
#define MB_FRAME_MODEL_H

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

#endif
