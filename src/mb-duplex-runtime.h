#ifndef MB_DUPLEX_RUNTIME_H
#define MB_DUPLEX_RUNTIME_H

#include "mb-runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

G_BEGIN_DECLS

typedef struct {
    MbRuntimeFlow rx;
    MbRuntimeFlow tx;
    bool initialized;
} MbDuplexRuntime;

void mb_duplex_runtime_init(MbDuplexRuntime *runtime,
                            MbRuntimeConsumeFunc rx_consume,
                            void *rx_user_data,
                            MbRuntimeConsumeFunc tx_consume,
                            void *tx_user_data);
void mb_duplex_runtime_clear(MbDuplexRuntime *runtime);

bool mb_duplex_runtime_start(MbDuplexRuntime *runtime);
void mb_duplex_runtime_stop(MbDuplexRuntime *runtime);

bool mb_duplex_runtime_push_rx(MbDuplexRuntime *runtime,
                               const uint8_t *data,
                               size_t len,
                               uint64_t timestamp_ns);
bool mb_duplex_runtime_push_rx_with_epoch(MbDuplexRuntime *runtime,
                                           const uint8_t *data,
                                           size_t len,
                                           uint64_t timestamp_ns,
                                           uint64_t epoch);
bool mb_duplex_runtime_push_tx(MbDuplexRuntime *runtime,
                               const uint8_t *data,
                               size_t len,
                               uint64_t timestamp_ns);
bool mb_duplex_runtime_push_tx_with_epoch(MbDuplexRuntime *runtime,
                                           const uint8_t *data,
                                           size_t len,
                                           uint64_t timestamp_ns,
                                           uint64_t epoch);
void mb_duplex_runtime_drop_pending(MbDuplexRuntime *runtime);

uint8_t mb_duplex_runtime_rx_depth(const MbDuplexRuntime *runtime);
uint8_t mb_duplex_runtime_tx_depth(const MbDuplexRuntime *runtime);

G_END_DECLS

#endif /* MB_DUPLEX_RUNTIME_H */