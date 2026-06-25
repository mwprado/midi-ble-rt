# midi-ble-rt v0.5.0

This release adds session statistics monitoring to the orchestrated BLE-MIDI data path.

## Highlights

- Add `mb-stats` core module for per-session runtime counters.
- Enable statistics by default through `[stats] enabled = yes`.
- Export the current session snapshot to:

  ```text
  /run/user/$UID/midi-ble-rt/stats.tsv
  ```

- Add `midi-ble-rtctl stats` for aligned human-readable statistics.
- Add `midi-ble-rtctl top` for a continuously refreshed operator view.
- Count RX packets, TX packets, RX/TX bytes, drops, last activity age, host-side gap averages/maxima, and RX/TX queue depth.
- Document statistics format and operational semantics in `docs/STATS.md`.

## Notes

The raw statistics file is TSV and intended for scripts/tools. The `midi-ble-rtctl stats` and `midi-ble-rtctl top` commands provide the aligned human-readable view.

The gap metrics are host-side timing gaps. They are not end-to-end musical latency measurements.

## Build/package

The Fedora spec is updated to package version `0.5.0` and expects the source tag:

```text
v0.5.0
```
