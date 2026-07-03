# Code cleanup plan

## Goal

Prepare the v1.0.0 codebase by removing obsolete compatibility code without
changing runtime behavior.

This cleanup is conservative: no protocol changes, no reconnect policy changes,
no ALSA routing changes and no RPM/spec changes.

## Rules

- No new features.
- No protocol changes.
- No ALSA behavior changes.
- No reconnect policy changes.
- No RPM/spec changes unless required by build breakage.
- Every cleanup step must compile and pass tests before merge.
- Prefer small commits with one responsibility each.
- Keep the validated release behavior as the reference.

## Cleanup status

Completed before this v1.0.0 cleanup branch:

- BlueZ helpers were extracted to `mb-bluez.[ch]`.
- BLE-MIDI GATT helpers were extracted to `mb-gatt-midi.[ch]`.
- ALSA port lifecycle helpers were extracted to `mb-alsa-port.[ch]`.
- The monolithic `src/midi-ble-rtd.c` source was removed.
- The hidden `#include "midi-ble-rtd.c"` pattern was removed.
- The temporary `mb-legacy-core.h` compatibility shim was removed.
- CI was added to build the project and run CTest on pushes and pull requests.

Completed in `cleanup-v1.0.0-remove-legacy`:

- Removed the legacy adaptive `mb-buffer.[ch]` module.
- Removed `MbSessionBuffers` from `MbSession`.
- Removed the obsolete `test-mb-buffer` target.
- Removed the legacy single-device ALSA wrapper API.
- Removed the obsolete `mb_stats_export_tsv()` v4 exporter.
- Kept the active v5 daemon stats exporter used by the multi-device runtime.

Remaining before merge:

- Build the branch with CMake.
- Run CTest.
- Run the physical GO:KEYS smoke test before merging to `master`.

## Current module split

- `mb-daemon-main.c`: daemon executable entry point and version handling.
- `mb-daemon.c`: multi-device daemon runtime, lifecycle queue and dataplane glue.
- `mb-bluez.[ch]`: BlueZ Device1, ObjectManager and property helpers.
- `mb-gatt-midi.[ch]`: BLE-MIDI service/characteristic discovery, notify and write helpers.
- `mb-alsa-port.[ch]`: shared ALSA client and per-device duplex port lifecycle.
- `mb-ble-midi.[ch]`: BLE-MIDI packet encode/decode.
- `mb-session.[ch]`: session state and invariants.
- `mb-runtime.[ch]`, `mb-duplex-runtime.[ch]`, `mb-slice-ring.[ch]`, `mb-frame-model.[ch]`: active threaded RX/TX dataplane queues.
- `mb-stats.[ch]`: stats counters and default path helpers.

## Validation

CI validation is defined in `.github/workflows/ci.yml` and runs:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Local validation before merge:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

Manual smoke test:

1. Start `midi-ble-rtd` with the GO:KEYS configuration.
2. Check `midi-ble-rtctl stats`.
3. Check `aconnect -l`.
4. Confirm `STREAMING` and a valid ALSA `client:port`.
5. Turn the keyboard off and confirm `RECONNECTING`.
6. Turn the keyboard on and confirm `STREAMING` again.

## Out of scope for this branch

- Latency and jitter optimization.
- Thread priority or real-time scheduling.
- Backoff policy changes.
- Multi-device behavior changes.
- RPM release work.
- CLI feature expansion.
