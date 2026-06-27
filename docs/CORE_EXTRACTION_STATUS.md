# Core extraction status

This document records the post-`v0.4.0` extraction state so new work does not
accidentally grow the remaining legacy core.

## Current public daemon

```text
midi-ble-rtd
  -> mb-daemon
      -> mb-orchestrator
          -> core modules / runtime queues / legacy core being extracted
```

`midi-ble-rtd` is the only public daemon.  The former `midi-ble-rtd-duplex`
validation wrapper has been removed.

## Extracted core modules

```text
mb-session          session state and invariants
mb-alsa             ALSA Sequencer event classification
mb-config           runtime config parsing and defaults
mb-ble-midi         BLE-MIDI UUIDs, status lengths and short packet helpers
mb-buffer           adaptive session buffers
mb-runtime          runtime flow primitives
mb-duplex-runtime   threaded RX/TX runtime
mb-slice-ring       fixed-size queue primitive
mb-frame-model      frame/model helpers
mb-log              structured logging
```

## Remaining legacy concentration

`src/midi-ble-rtd.c` is still included by `mb-orchestrator.c` during the ongoing
extraction.  It still contains static helpers for BlueZ, GATT, ALSA I/O and the
BLE-MIDI RX parser.

Do not add new feature logic to `src/midi-ble-rtd.c`.

## Remaining extraction targets

Recommended order:

```text
1. mb-bluez
   Device1 discovery, pair/trust/connect, ServicesResolved polling

2. mb-gatt
   BLE-MIDI service discovery, characteristic discovery, StartNotify, WriteValue

3. mb-ble-midi RX parser
   BLE-MIDI notification packet parsing with callback-based MIDI byte output

4. mb-alsa I/O
   ALSA Sequencer client/port creation and MIDI byte encode/decode ownership
```

## Rule for new functionality

New functionality should be implemented in one of these places:

```text
mb-orchestrator     runtime/session policy
explicit core module low-level domain operation
midi-ble-rtctl      user-facing control plane
```

Avoid this pattern:

```text
new feature -> src/midi-ble-rtd.c
```

Preferred pattern:

```text
new feature -> mb-orchestrator -> core module
```

## Next feature candidate

The next high-value feature is controlled reconnect:

```text
BlueZ disconnect or notify/write failure
  -> session leaves STREAMING
  -> reconnect policy decides retry/backoff/give-up
  -> Device1 connect
  -> wait ServicesResolved
  -> rebind BLE-MIDI GATT characteristic
  -> StartNotify
  -> return to STREAMING
```

This feature should be implemented in `mb-orchestrator` first and only extract
more BlueZ/GATT helpers when the boundary is stable.
