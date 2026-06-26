# midi-ble-rt v0.6.2

Release focused on internal cleanup and operational telemetry.

## Changes

- Internal code cleanup after the validated RPM release.
- BlueZ helper extraction.
- BLE-MIDI GATT helper extraction.
- ALSA port lifecycle helper extraction.
- Legacy daemon boundary reduced.
- Stats TSV format bumped to v3.
- ALSA RX and ALSA TX are now exported as separate fields.
- midi-ble-rtctl stats/top now displays:
  - ALSA RX: <client>:<port>
  - ALSA TX: <client>:<port>

## Notes

The current ALSA port remains duplex, so RX and TX may show the same
client:port today. The telemetry model no longer assumes they must always
be the same port.

## Validation

- Build OK.
- ctest OK.
- stats/top OK.
- Existing BLE-MIDI runtime behavior preserved.
