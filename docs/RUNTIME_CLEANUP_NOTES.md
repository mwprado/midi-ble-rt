# Runtime cleanup notes

This note records small follow-up work after the multi-session ALSA TX polling change.

## Current dataplane shape

Control-plane remains on the GLib main loop and lifecycle queue.

RX path:

```text
BlueZ GATT notification
  -> per-device RX ring
  -> midi-ble-rt-rx worker
  -> BLE-MIDI decode
  -> ALSA Sequencer output
```

TX path:

```text
global ALSA poll thread: midi-ble-rt-als
  -> dispatch by ALSA destination port
  -> per-device TX ring
  -> midi-ble-rt-tx worker
  -> BLE-MIDI packet
  -> BlueZ GATT WriteValue
```

## Cleanup follow-ups

Keep these changes small and separate from dataplane fixes.

1. Reduce periodic health/recheck log noise. Normal logs should report state changes, failures, stream start/stop, and thread start/stop. Repeated healthy rechecks should be debug-only or silent.
2. Expose ALSA TX thread state in `midi-ble-rtctl daemon-status`, for example `alsa_tx_thread=running`.
3. Add tests for repeated ALSA TX thread start, `enable_tx=no`, shutdown join, unknown ALSA destination port, and non-MIDI ALSA events.
4. Keep `mb-orchestrator.c` as compatibility/single-file path only. Do not add new multi-session dataplane logic there.
5. Consider later extraction from `mb-daemon.c` only after the multi-session runtime is stable: lifecycle runner, device runtime, control socket, and ALSA TX thread can become separate modules.

## Naming convention

Use TX for ALSA-to-BLE and RX for BLE-to-ALSA.

Avoid names such as `alsa_rx_poll` for code that reads ALSA input and sends data to BLE. Prefer names like `runtime_drain_alsa_tx_queue` and `runtime_alsa_tx_thread_main`.
