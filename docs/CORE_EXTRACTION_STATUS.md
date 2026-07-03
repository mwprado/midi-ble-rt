# Core extraction status

This document records the v1.0.0 cleanup state so new work does not reintroduce
legacy daemon paths.

## Current public daemon

```text
midi-ble-rtd
  -> mb-daemon-main.c
      -> mb-daemon.c
          -> core modules / runtime queues
```

`midi-ble-rtd` is the only public daemon. The former validation wrappers and the
former monolithic daemon source were removed before v1.0.0 cleanup.

## Extracted core modules

```text
mb-session          session state and invariants
mb-alsa             ALSA Sequencer event classification
mb-alsa-port        shared ALSA client and per-device duplex ports
mb-config           runtime config parsing and defaults
mb-bluez            BlueZ Device1/ObjectManager/Properties helpers
mb-gatt-midi        BLE-MIDI service/characteristic discovery and GATT I/O
mb-ble-midi         BLE-MIDI packet encode/decode
mb-runtime          runtime flow primitive
mb-duplex-runtime   threaded RX/TX runtime wrapper
mb-slice-ring       active fixed-size runtime queue
mb-frame-model      active runtime frame pool
mb-stats            stats counters and default path helpers
mb-log              structured logging
mb-paths            runtime path helpers
mb-rtkit            optional RTKit integration
```

## Removed legacy code

Removed before or during the v1.0.0 cleanup branch:

```text
src/midi-ble-rtd.c              former monolithic daemon
mb-legacy-core.h                temporary compatibility shim
mb-buffer.[ch]                  unused adaptive session buffer prototype
MbSessionBuffers                obsolete per-session buffer fields
test-mb-buffer                  tests for removed buffer prototype
mb_alsa_port_open_duplex()      single-device ALSA wrapper
mb_alsa_port_close()            single-device ALSA wrapper
mb_stats_export_tsv()           obsolete stats.tsv v4 exporter
```

## Rule for new functionality

New functionality should be implemented in one of these places:

```text
mb-daemon.c          runtime/session policy and dataplane glue
explicit core module low-level domain operation
midi-ble-rtctl       user-facing control plane
```

Avoid reintroducing:

```text
single-device daemon compatibility path
hidden #include of implementation .c files
parallel BLE-MIDI decoders
parallel ALSA port lifecycle wrappers
unused queue/buffer abstractions outside the active dataplane
```

## Current dataplane

```text
BLE notification
  -> mb_duplex_runtime_push_rx_with_epoch()
  -> mb-runtime / mb-slice-ring / mb-frame-model
  -> mb_ble_midi_decode_packet()
  -> ALSA Sequencer output

ALSA Sequencer input
  -> mb_duplex_runtime_push_tx_with_epoch()
  -> mb-runtime / mb-slice-ring / mb-frame-model
  -> mb_ble_midi_make_packet()
  -> GATT WriteValue(command)
```

The active runtime queue is `mb-slice-ring` over `mb-frame-model`. Do not add new
runtime behavior to the removed adaptive `mb-buffer` model.
