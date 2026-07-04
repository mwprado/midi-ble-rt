# midi-ble-rt v0.9.4

v0.9.4 is a cleanup, diagnostics and documentation release preparing the codebase for v1.0.0.

## Highlights

- Removed legacy runtime/session cleanup code that was no longer used by the multi-device daemon path.
- Exposed RTKit state in `midi-ble-rtctl daemon-status`:
  - `rtkit`
  - `rtkit_priority`
  - `rtkit_rx`
  - `rtkit_tx`
- Documented all `daemon-status` output fields in CLI help.
- Added Linux manual pages:
  - `midi-ble-rtd(1)`
  - `midi-ble-rtctl(1)`
- Added GNU Info manual source and optional `makeinfo` integration in CMake.
- Updated Fedora packaging to build and package man/Info documentation with `texinfo`.
- Fixed an ordering race in `mb-runtime`: the internal `flow->consumed` counter is now advanced before invoking the consumer callback, so observers woken from inside the callback no longer see the runtime counter lagging by one item.

## Behavior changes

No intentional BLE-MIDI protocol, ALSA routing, reconnect policy or GATT behavior changes are included in this release.

## Validation recommended before publishing packages

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure

for i in $(seq 1 50); do
  ctest --test-dir build -R mb-runtime --output-on-failure || exit 1
done

DESTDIR=/tmp/midi-ble-rt-install cmake --install build
packaging/fedora/make-srpm.sh
```
