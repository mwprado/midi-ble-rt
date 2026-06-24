#include "mb-duplex-runtime.h"

#include <string.h>

void mb_duplex_runtime_init(MbDuplexRuntime *runtime,
                            MbRuntimeConsumeFunc rx_consume,
                            void *rx_user_data,
                            MbRuntimeConsumeFunc tx_consume,
                            void *tx_user_data) {
    if (!runtime)
        return;

    memset(runtime, 0, sizeof(*runtime));

    mb_runtime_flow_init(&runtime->rx,
                         "midi-ble-rt-rx",
                         MB_SLICE_RING_RX_RAW,
                         rx_consume,
                         rx_user_data);
    mb_runtime_flow_init(&runtime->tx,
                         "midi-ble-rt-tx",
                         MB_SLICE_RING_TX_RAW,
                         tx_consume,
                         tx_user_data);
    runtime->initialized = true;
}

void mb_duplex_runtime_clear(MbDuplexRuntime *runtime) {
    if (!runtime || !runtime->initialized)
        return;

    mb_duplex_runtime_stop(runtime);
    mb_runtime_flow_clear(&runtime->rx);
    mb_runtime_flow_clear(&runtime->tx);
    memset(runtime, 0, sizeof(*runtime));
}

bool mb_duplex_runtime_start(MbDuplexRuntime *runtime) {
    if (!runtime || !runtime->initialized)
        return false;

    if (!mb_runtime_flow_start(&runtime->rx))
        return false;

    if (!mb_runtime_flow_start(&runtime->tx)) {
        mb_runtime_flow_stop(&runtime->rx);
        return false;
    }

    return true;
}

void mb_duplex_runtime_stop(MbDuplexRuntime *runtime) {
    if (!runtime || !runtime->initialized)
        return;

    mb_runtime_flow_stop(&runtime->rx);
    mb_runtime_flow_stop(&runtime->tx);
}

bool mb_duplex_runtime_push_rx(MbDuplexRuntime *runtime,
                               const uint8_t *data,
                               size_t len,
                               uint64_t timestamp_ns) {
    return runtime && runtime->initialized &&
           mb_runtime_flow_push_bytes(&runtime->rx, data, len, timestamp_ns);
}

bool mb_duplex_runtime_push_tx(MbDuplexRuntime *runtime,
                               const uint8_t *data,
                               size_t len,
                               uint64_t timestamp_ns) {
    return runtime && runtime->initialized &&
           mb_runtime_flow_push_bytes(&runtime->tx, data, len, timestamp_ns);
}

uint8_t mb_duplex_runtime_rx_depth(const MbDuplexRuntime *runtime) {
    return runtime && runtime->initialized ? mb_runtime_flow_depth(&runtime->rx) : 0;
}

uint8_t mb_duplex_runtime_tx_depth(const MbDuplexRuntime *runtime) {
    return runtime && runtime->initialized ? mb_runtime_flow_depth(&runtime->tx) : 0;
}
