# Code cleanup plan

## Goal

Reduce technical debt after the validated RPM release without changing runtime behavior.

This branch is for conservative cleanup only. Feature work belongs in later branches.

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

Completed in the cleanup series:

- BlueZ helpers were extracted to `mb-bluez.[ch]`.
- BLE-MIDI GATT helpers were extracted to `mb-gatt-midi.[ch]`.
- ALSA port lifecycle helpers were extracted to `mb-alsa-port.[ch]`.
- Shared daemon app/context helpers were moved to `mb-app.[ch]`.
- The monolithic `src/midi-ble-rtd.c` source was removed.
- The hidden `#include "midi-ble-rtd.c"` pattern was removed.
- `mb-orchestrator.c` now calls the `mb_app_*` API directly.
- The temporary `mb-legacy-core.h` compatibility shim was removed.
- CI was added to build the project and run CTest on pushes and pull requests.

Remaining before merge:

- Check GitHub Actions results for `remove-legacy-core` after the final cleanup commits.
- Run the physical GO:KEYS smoke test before merging to `master`.

## Target module split

- `mb-bluez.[ch]`: BlueZ Device1, ObjectManager and property helpers.
- `mb-gatt-midi.[ch]`: BLE-MIDI service/characteristic discovery, notify and write helpers.
- `mb-alsa-port.[ch]`: ALSA Sequencer client/port lifecycle.
- `mb-app.[ch]`: daemon app context, config bridge, discovery glue and BLE-MIDI decode to ALSA.
- `mb-daemon.c`: thin process entrypoint.
- `mb-orchestrator.c`: session orchestration, lifecycle and state transitions.

## Validation

CI validation is defined in `.github/workflows/ci.yml` and runs:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Local validation before merge:

```sh
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
