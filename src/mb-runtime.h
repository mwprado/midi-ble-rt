#ifndef MB_RUNTIME_H
#define MB_RUNTIME_H

#include "mb-slice-ring.h"

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

G_BEGIN_DECLS

typedef struct _MbRuntimeFlow MbRuntimeFlow;

typedef void (*MbRuntimeConsumeFunc)(MbRuntimeFlow *flow,
                                     const MbSliceRingItem *item,
                                     const uint8_t *data,
                                     size_t len,
                                     void *user_data);

typedef struct {
    uint64_t pushed;
    uint64_t consumed;
    uint64_t dropped;
    uint64_t push_failures;
    uint64_t overflows;
    uint8_t depth;
} MbRuntimeFlowStats;

struct _MbRuntimeFlow {
    const char *name;

    MbFrameModelPool pool;
    MbSliceRing ring;

    GMutex wake_lock;
    GCond wake_cond;
    bool wake_initialized;

    GThread *consumer_thread;
    bool stop_requested;

    MbRuntimeConsumeFunc consume;
    void *user_data;

    uint64_t pushed;
    uint64_t consumed;
    uint64_t dropped;
    uint64_t push_failures;
};

void mb_runtime_flow_init(MbRuntimeFlow *flow,
                          const char *name,
                          MbSliceRingKind ring_kind,
                          MbRuntimeConsumeFunc consume,
                          void *user_data);
void mb_runtime_flow_clear(MbRuntimeFlow *flow);

bool mb_runtime_flow_start(MbRuntimeFlow *flow);
void mb_runtime_flow_stop(MbRuntimeFlow *flow);

bool mb_runtime_flow_push_bytes(MbRuntimeFlow *flow,
                                const uint8_t *data,
                                size_t len,
                                uint64_t timestamp_ns);
void mb_runtime_flow_wake(MbRuntimeFlow *flow);

uint8_t mb_runtime_flow_depth(const MbRuntimeFlow *flow);
uint64_t mb_runtime_flow_overflows(const MbRuntimeFlow *flow);
MbRuntimeFlowStats mb_runtime_flow_stats(const MbRuntimeFlow *flow);

G_END_DECLS

#endif /* MB_RUNTIME_H */
