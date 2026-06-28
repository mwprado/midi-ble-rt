# midi-ble-rt v0.7.0

Architectural release for the config-directory multi-session runtime.

## Changes

- Serialize lifecycle/control-plane operations through the GLib main loop monitor.
- Route lifecycle state changes through the monitor.
- Keep callbacks and dataplane workers from applying lifecycle state changes directly.
- Keep RX/TX dataplane workers parallel per BLE-MIDI device after STREAMING.
- Replace the global GATT write lock with per-device GATT TX locking.
- Preserve the ALSA Sequencer to BLE-MIDI TX path.
- Preserve the BLE-MIDI to ALSA Sequencer RX path.

## Architecture

The daemon now separates control-plane and dataplane responsibilities:

- The main loop owns lifecycle/control-plane operations:
  - discovery
  - connect
  - disconnect
  - health/recheck
  - reconnect
  - session state transitions
- Each STREAMING BLE-MIDI device owns its dataplane workers:
  - one RX worker
  - one TX worker
  - one per-device GATT TX lock

This prepares the runtime for multiple BLE-MIDI instruments or controllers playing simultaneously.

## Validation

- Build OK.
- Runtime validated with real BLE-MIDI hardware.
- ALSA to BLE-MIDI TX validated with `aplaymidi`.
- BLE-MIDI to ALSA RX reported functional.

## Known pending work

- Export stats/top from the config-directory multi-session runtime.
- Route future `midi-ble-rtctl` connect/disconnect/recheck commands through the daemon monitor via IPC.
- Validate simultaneous multi-device operation with two BLE-MIDI devices.
