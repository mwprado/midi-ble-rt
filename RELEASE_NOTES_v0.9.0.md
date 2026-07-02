# midi-ble-rt v0.9.0

## Summary

v0.9.0 completes the BLE-MIDI dataplane hardening work and removes the legacy single-file configuration path.

The daemon now uses the directory-based multi-session runtime as the only supported runtime path. This simplifies the architecture around one daemon model, one session/dataplane policy, and one configuration layout.

## Highlights

- Added dataplane epoch fencing.
- Dropped stale RX/TX queue items when a device leaves STREAMING.
- Reset partial BLE-MIDI and ALSA MIDI state across session boundaries.
- Made the BLE-MIDI decoder stateful across packets.
- Added support for fragmented short MIDI messages.
- Added support for running status across BLE packet boundaries.
- Added support for fragmented SysEx streams.
- Preserved pending MIDI state when realtime bytes are interleaved.
- Added fail-closed resync behavior for ambiguous BLE-MIDI framing.
- Added MIDI panic on unsafe RX resync to avoid stuck notes, sustain or hanging sound.
- Documented BLE-MIDI fragmentation and resync policy.
- Removed the legacy single-file configuration path.

## Configuration change

The daemon no longer supports legacy single-file configuration.

Supported forms are now directory-based:

```bash
midi-ble-rtd --config ~/.config/midi-ble-rt
midi-ble-rtd --config-dir ~/.config/midi-ble-rt
