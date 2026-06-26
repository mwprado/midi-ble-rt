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
- Every cleanup step must compile and pass tests.
- Prefer small commits with one responsibility each.
- Keep the validated release behavior as the reference.

## Current debt

- `mb-orchestrator.c` includes `midi-ble-rtd.c` by redefining `main`.
- BlueZ/GATT helper functions are coupled to the legacy daemon file.
- ALSA port lifecycle is mixed with BLE/GATT session orchestration.
- Runtime dataplane and session state transitions are close together.
- Stats export is operationally useful but tightly coupled to orchestrator state.
- `mb-orchestrator.c` is carrying too many responsibilities.

## Target module split

- `mb-bluez.[ch]`: BlueZ Device1, ObjectManager and property helpers.
- `mb-gatt-midi.[ch]`: BLE-MIDI service/characteristic discovery, notify and write helpers.
- `mb-alsa-port.[ch]`: ALSA Sequencer client/port lifecycle.
- `mb-orchestrator.c`: session orchestration, lifecycle and state transitions.
- `midi-ble-rtd.c`: thin daemon entrypoint.

## Cleanup sequence

### Phase 1: map and stabilize

1. Document responsibilities.
2. Identify helper functions that are pure or nearly pure.
3. Avoid moving behavior and changing logic in the same commit.

### Phase 2: extract BlueZ helpers

1. Move ObjectManager and Device1 property helpers out of `midi-ble-rtd.c`.
2. Keep signatures close to the current `App`-based API first.
3. Only after tests pass, refine the interface.

### Phase 3: extract BLE-MIDI GATT binding

1. Move BLE-MIDI service discovery.
2. Move BLE-MIDI characteristic selection.
3. Move StartNotify and WriteValue wrappers.

### Phase 4: extract ALSA port lifecycle

1. Move ALSA Sequencer client/port creation.
2. Keep MIDI encode/decode ownership explicit.
3. Preserve dynamic ALSA `client:port` behavior.

### Phase 5: simplify orchestrator

1. Remove the pattern that includes `midi-ble-rtd.c` by redefining `main`.
2. Keep `mb-orchestrator.c` focused on state and runtime lifecycle.
3. Keep dataplane error policy explicit and testable.

## Validation

After every cleanup commit, build the project and run the CTest suite.

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
