# v0.4.0

## Summary

`v0.4.0` consolidates the daemon architecture around one public runtime entry
point: `midi-ble-rtd`.

The experimental duplex validation path has been absorbed into an internal
orchestrator layer.  RX and TX now run through the single daemon model, while the
codebase starts extracting reusable core helpers for future maintenance and
multi-session work.

## Added

- `mb-daemon` process entry layer.
- `mb-orchestrator` runtime/session orchestration layer.
- Single public daemon architecture documented in `docs/ARCHITECTURE.md`.
- ALSA Sequencer event classification helper module: `mb-alsa`.
- Unit tests for ALSA MIDI payload events versus Sequencer control events.

## Changed

- `midi-ble-rtd` now uses the orchestrated RX/TX runtime path.
- The daemon startup now reports `Runtime: orchestrator`.
- `midi-ble-rtd-duplex` is no longer built or installed as a separate daemon
  target.
- Documentation now describes the internal split:

```text
midi-ble-rtd
  -> mb-daemon
      -> mb-orchestrator
          -> core modules / session model / runtime queues
```

## Fixed

- ALSA Sequencer control events such as `PORT_SUBSCRIBED` and
  `PORT_UNSUBSCRIBED` are classified as non-MIDI payload before MIDI decode.
  This prevents spurious `snd_midi_event_decode()` failures for event types 66
  and 67.

## Packaging notes

The Fedora package installs the public binaries:

```text
/usr/bin/midi-ble-rtd
/usr/bin/midi-ble-rtctl
```

It does not install `midi-ble-rtd-duplex`.

## Validation checklist

Before publishing RPM builds, validate:

```bash
cmake --build build -j8
ctest --test-dir build --output-on-failure
./build/midi-ble-rtd --config ~/.config/midi-ble-rt/roland-gokeys.ini
```

Expected runtime marker:

```text
midi-ble-rtd
Runtime: orchestrator
```

Hardware validation target:

```text
Roland GO:KEYS RX and TX through ALSA Sequencer
```
