# v0.2.0

## Summary

Stabilizes the internal MIDI session model and prepares the daemon architecture
for multiple BLE-MIDI keyboards.

This release is intended as a new stability checkpoint for RPM packaging and
single-keyboard GO:KEYS validation. It does not claim full multi-keyboard hardware
validation.

## Added

- `MbSession` state model.
- `MbDaemon` session registry.
- Per-device session identity by Bluetooth address and BlueZ device path.
- Unit tests for session transitions and multi-session behavior.
- Developer documentation in `DEVELOPERS.md`.
- State diagrams for MIDI session lifecycle.
- Documentation for identical keyboards with different Bluetooth addresses.
- Generic standards-only BLE-MIDI config example: `config/standard-ble-midi.ini.example`.

## Changed

- README simplified for the user-facing workflow.
- Architecture documentation moved to developer README.
- Project model clarified:
  - BlueZ owns Bluetooth/GATT state.
  - `midi-ble-rt` owns MIDI session state.
  - ALSA remains the musical endpoint.
  - PipeWire/WirePlumber and apps consume the exported ALSA MIDI ports.
- Config examples are installed under `%{_datadir}/midi-ble-rt/config`.

## Validation scope

Validated target remains the Roland GO:KEYS single-device workflow.

The generic BLE-MIDI config is provided for testing standards-compliant devices,
but validation on additional physical keyboards is still pending.

Multi-session behavior is covered by unit tests and architecture documentation,
but has not yet been validated with two physical BLE-MIDI keyboards.

## Suggested RPM version

```spec
Version: 0.2.0
Release: 1%{?dist}
```
