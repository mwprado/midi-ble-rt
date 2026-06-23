#ifndef MB_BUFFER_H
#define MB_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MB_BUFFER_SLOT_COUNT          16u
#define MB_BUFFER_SLOT_CAPACITY       256u
#define MB_BUFFER_LIMIT_MIN           64u
#define MB_BUFFER_LIMIT_MAX           256u
#define MB_BUFFER_SHRINK_THRESHOLD    32u
#define MB_BUFFER_SHRINK_AFTER_NS     (30ULL * 1000ULL * 1000ULL * 1000ULL)
#define MB_BUFFER_DEPTH_PRESSURE      4u

typedef enum {
    MB_BUFFER_RX_RAW = 0,
    MB_BUFFER_RX_DECODED,
    MB_BUFFER_RX_ACTIVE,
    MB_BUFFER_TX_RAW,
    MB_BUFFER_TX_DECODED,
    MB_BUFFER_TX_ACTIVE,
    MB_BUFFER_KIND_COUNT
} MbBufferKind;

typedef struct {
    uint8_t data[MB_BUFFER_SLOT_CAPACITY];
    uint16_t len;
    uint64_t seq;
    uint64_t timestamp_ns;
    uint32_t flags;
} MbBufferSlot;

typedef struct {
    MbBufferKind kind;

    /* Fixed circular array. No heap allocation on the hot path. */
    MbBufferSlot slots[MB_BUFFER_SLOT_COUNT];
    uint8_t read_idx;
    uint8_t write_idx;
    uint8_t count;

    uint16_t active_limit;
    uint16_t max_slot_len;
    uint8_t max_depth;

    uint64_t next_seq;
    uint64_t pushed;
    uint64_t popped;
    uint64_t overflows;
    uint64_t oversize_drops;
    uint64_t grow_count;
    uint64_t shrink_count;

    uint64_t last_pressure_ns;
} MbBufferRing;

typedef struct {
    MbBufferRing raw;
    MbBufferRing decoded;
    MbBufferRing active;
} MbBufferPipeline;

typedef struct {
    MbBufferPipeline rx;
    MbBufferPipeline tx;
} MbSessionBuffers;

const char *mb_buffer_kind_name(MbBufferKind kind);

void mb_buffer_ring_init(MbBufferRing *ring, MbBufferKind kind, uint64_t now_ns);
void mb_buffer_ring_reset(MbBufferRing *ring, uint64_t now_ns);

bool mb_buffer_ring_push(MbBufferRing *ring,
                         const uint8_t *data,
                         size_t len,
                         uint64_t timestamp_ns,
                         uint64_t now_ns);
bool mb_buffer_ring_peek(const MbBufferRing *ring, const MbBufferSlot **slot);
bool mb_buffer_ring_pop(MbBufferRing *ring, MbBufferSlot *out_slot);

void mb_buffer_ring_tick(MbBufferRing *ring, uint64_t now_ns);

bool mb_buffer_ring_is_empty(const MbBufferRing *ring);
bool mb_buffer_ring_is_full(const MbBufferRing *ring);

uint16_t mb_buffer_ring_active_limit(const MbBufferRing *ring);
uint8_t mb_buffer_ring_count(const MbBufferRing *ring);
uint64_t mb_buffer_ring_overflows(const MbBufferRing *ring);

void mb_buffer_pipeline_init(MbBufferPipeline *pipeline,
                             MbBufferKind raw_kind,
                             MbBufferKind decoded_kind,
                             MbBufferKind active_kind,
                             uint64_t now_ns);
void mb_buffer_pipeline_reset(MbBufferPipeline *pipeline,
                              MbBufferKind raw_kind,
                              MbBufferKind decoded_kind,
                              MbBufferKind active_kind,
                              uint64_t now_ns);
void mb_buffer_pipeline_tick(MbBufferPipeline *pipeline, uint64_t now_ns);
uint64_t mb_buffer_pipeline_total_overflows(const MbBufferPipeline *pipeline);
uint16_t mb_buffer_pipeline_max_active_limit(const MbBufferPipeline *pipeline);

void mb_session_buffers_init(MbSessionBuffers *buffers, uint64_t now_ns);
void mb_session_buffers_reset(MbSessionBuffers *buffers, uint64_t now_ns);
void mb_session_buffers_tick(MbSessionBuffers *buffers, uint64_t now_ns);

uint64_t mb_session_buffers_total_overflows(const MbSessionBuffers *buffers);
uint16_t mb_session_buffers_max_active_limit(const MbSessionBuffers *buffers);

#endif /* MB_BUFFER_H */
