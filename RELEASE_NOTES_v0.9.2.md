# Release notes - v0.9.2

`v0.9.2` adds controlled RtKit realtime scheduling, compiled version reporting and
latency evaluation tooling on top of the `v0.9.1` measurement release.

## Highlights

- Add compiled version reporting:
  - `midi-ble-rtd -v`
  - `midi-ble-rtd -vv`
  - `midi-ble-rtctl -v`
  - `midi-ble-rtctl -vv`
- Generate the project version from CMake at build time.
- Pass the RPM spec version into CMake with `-DMIDI_BLE_RT_VERSION=%{version}`.
- Add optional RtKit scheduling through RealtimeKit.
- Add directional realtime worker selection:
  - `realtime_rx`
  - `realtime_tx`
- Keep RtKit globally disabled by default.
- When RtKit is enabled, default to RX realtime enabled and TX realtime disabled.
- Add latency capture and comparison helpers.
- Document the GO:KEYS latency and RtKit validation results.

## Runtime defaults

Default realtime configuration:

```ini
[realtime]
rtkit = no
rt_priority = 1
realtime_rx = yes
realtime_tx = no
```

Meaning:

- RtKit is opt-in.
- RX is the preferred realtime direction when the user enables RtKit.
- TX remains normal scheduling by default because the GO:KEYS `aplaymidi` test
  showed worse TX worst-case latency and jitter when TX used RtKit.

## Version source

Local build example:

```bash
cmake -S . -B build -DMIDI_BLE_RT_VERSION=0.9.2
cmake --build build
```

Fedora RPM build uses:

```spec
-DMIDI_BLE_RT_VERSION=%{version}
```

The generated build header is:

```text
build/generated/mb-version.h
```

## Validation focus

Recommended post-install checks:

```bash
midi-ble-rtd -v
midi-ble-rtd -vv
midi-ble-rtctl -v
midi-ble-rtctl -vv
midi-ble-rtctl --help
midi-ble-rtctl latency --help
midi-ble-rtctl latency-top --help
```

Recommended RtKit test configuration:

```ini
[stats]
enabled = yes
interval_ms = 1000
latency_diagnostics = yes

[realtime]
rtkit = yes
rt_priority = 1
realtime_rx = yes
realtime_tx = no
```

Expected log behavior:

```text
RtKit realtime scheduling enabled for midi-ble-rt-rx ...
```

There should be no TX RtKit enable message unless `realtime_tx = yes` is set
explicitly.

## Documentation updates

- `docs/LATENCY_TESTING.md` describes latency capture, summarization and
  acceptance criteria.
- `docs/GOKEYS_TESTS.md` records the GO:KEYS validation run and resulting RtKit
  policy.
- `docs/MULTI_DEVICE_CONFIG.md` describes the directory-based multi-session
  runtime.
- `docs/SESSION_RESILIENCE.md` describes reconnect behavior and current timeout
  policy.

## Known follow-up work

- Expose RtKit configuration in `midi-ble-rtctl daemon-status`.
- Validate `v0.9.2` from the installed Fedora package, not only the local build.
- Run a clean RX-only RtKit latency capture from the installed package.
- Add more parser tests for fragmented SysEx, realtime interleaving, running
  status and fail-closed resync behavior.
- Continue Debian/Ubuntu packaging work.
