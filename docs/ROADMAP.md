# Project roadmap

This roadmap tracks `midi-ble-rt` after the `v0.6.3` cleanup and RPM packaging baseline.

`midi-ble-rt` is a Linux BLE-MIDI/GATT to ALSA Sequencer bridge. Roland GO:KEYS is the first validated hardware target, but the project target is any usable BLE-MIDI instrument, controller, module or adapter.

The project should evolve in this order:

```text
release hygiene first
generic BLE-MIDI validation second
multi-device runtime third
complete control CLI fourth
stable 1.0 after packaging, service mode and device validation are boring
low-jitter/realtime and polished duplex after the basics are stable
```

## Current baseline — v0.6.3 / RPM 0.6.3-2

Status: current repository baseline.

Done:

```text
master consolidated
pending cleanup branches merged or retired
v0.6.3 tag aligned with master
Fedora spec Version/Release/changelog aligned at 0.6.3-2
midi-ble-rt-core kept as an internal statically linked implementation library
RPM check rejects runtime dependency on private libmidi-ble-rt-core.so
remove-legacy-core cleanup incorporated
remove-duplex-wrapper cleanup incorporated
session-stats-top incorporated
generic BLE-MIDI device tutorial added
Roland GO:KEYS treated as validated hardware, not as the only project target
```

Known baseline capabilities:

```text
BlueZ system D-Bus connection
Device1 selection
trust/connect flow
ServicesResolved wait
GATT service discovery
BLE-MIDI service and characteristic selection
StartNotify data path
ALSA Sequencer port
BLE-MIDI RX decode
basic ALSA -> BLE-MIDI TX path
runtime statistics in stats.tsv
midi-ble-rtctl stats/top
Fedora RPM packaging
user systemd unit packaging
```

Immediate invariant:

```text
Installed executables must not require libmidi-ble-rt-core.so.
```

Validation command:

```bash
rpm -qp --requires midi-ble-rt-0.6.3-2*.rpm | grep midi-ble-rt-core
```

Expected result: no output.

## Immediate release hygiene

Goal: make `v0.6.3` reproducible and unambiguous as a packaged release.

Tasks:

```text
confirm COPR builds midi-ble-rt-0.6.3-2
confirm RPM requires do not include libmidi-ble-rt-core.so
confirm regular Fedora installation
confirm rpm-ostree installation
confirm service files are installed in the expected user unit path
run post-install smoke test with a real BLE-MIDI device
record any COPR/rpm-ostree notes in documentation
```

Exit criteria:

```text
COPR publishes 0.6.3-2 for the target Fedora release
RPM installs without private core-library dependency errors
midi-ble-rtd --version or --help works from the installed package
midi-ble-rtctl --help works from the installed package
basic runtime smoke test succeeds with the validated GO:KEYS setup
```

Version target: `v0.6.3` release closure, then `v0.6.4` only if packaging fixes are required.

## v0.7 — Generic BLE-MIDI device validation

Goal: validate the standard BLE-MIDI path beyond Roland GO:KEYS.

The generic profile should prefer the official BLE-MIDI UUIDs:

```text
service_uuid = 03b80e5a-ede8-4b33-a751-6ce34ec4c700
io_uuid      = 7772e5db-3868-4112-a1a9-f2669d106bf3
```

Tasks:

```text
keep standard-ble-midi.ini as the main generic example
validate one non-Roland BLE-MIDI device
separate standard_ble_midi behavior from roland_gokeys quirks
keep Roland-specific UUID aliases out of the generic profile
replace user-facing keyboard-only wording with device/instrument/controller/module/adapter wording
add a small compatibility matrix
make profile selection visible in probe/status output where useful
```

Exit criteria:

```text
Roland GO:KEYS still works
one standard non-Roland BLE-MIDI device works without vendor-specific alias
standard BLE-MIDI tutorial matches the tested flow
README describes the project as generic BLE-MIDI infrastructure
```

Version target: `v0.7`.

## v0.8 — Multi-device runtime

Goal: run more than one BLE-MIDI device at the same time.

Model:

```text
one daemon process
multiple DeviceSession objects
one GATT notify subscription per device
one ALSA port per device
one profile/quirk policy per device
```

Expected ALSA shape:

```text
client 128: 'midi-ble-rt' [type=user]
    0 'Roland GO:KEYS BLE-MIDI'
    1 'Standard BLE-MIDI Device'
    2 'CME WIDI Master BLE-MIDI'
```

Configuration direction:

```ini
[device.roland-gokeys]
address = CB:81:F4:62:FF:07
profile = roland_gokeys
autoconnect = yes
alsa_port_name = Roland GO:KEYS BLE-MIDI

[device.standard-1]
address = AA:BB:CC:DD:EE:FF
profile = standard_ble_midi
autoconnect = no
alsa_port_name = Standard BLE-MIDI Device
```

Tasks:

```text
validate two simultaneous BLE-MIDI devices
keep device identity address-based, not name-based
expose one ALSA port per device
keep one device failure isolated from the others
show per-device state in stats/top/status
document naming policy for identical or renamed devices
```

Exit criteria:

```text
two configured devices produce two ALSA ports
one device can disconnect/reconnect without killing the other
CLI status shows each session independently
reconnect preserves stable identity where possible
```

Version target: `v0.8`.

## v0.9 — Complete control CLI

Goal: make normal operation possible without manually driving `bluetoothctl`.

Required commands:

```bash
midi-ble-rtctl scan --midi-only
midi-ble-rtctl list --midi-only
midi-ble-rtctl info DEVICE
midi-ble-rtctl probe DEVICE
midi-ble-rtctl connect DEVICE
midi-ble-rtctl disconnect DEVICE
midi-ble-rtctl forget DEVICE
midi-ble-rtctl pair DEVICE
midi-ble-rtctl trust DEVICE
midi-ble-rtctl untrust DEVICE
midi-ble-rtctl status
midi-ble-rtctl stats
midi-ble-rtctl top
```

Behavior:

```text
scan nearby devices
show address/name/alias/RSSI/UUID hints
show known BlueZ devices
connect temporarily for GATT inspection when probing
infer possible profile/quirk
print selected BLE-MIDI service and characteristic
apply pair/trust/connect policy through profile rules
report useful next-action errors
```

Exit criteria:

```text
user can discover a BLE-MIDI device without bluetoothctl
user can validate GATT service and characteristic
user can connect a known device through profile policy
errors explain the next corrective action
```

Version target: `v0.9`.

## v1.0 — Stable public release

Goal: stable public release for normal Linux/Fedora use.

Minimum requirements:

```text
Fedora RPM is reproducible through COPR
no runtime dependency on private internal libraries
regular Fedora install works
rpm-ostree install works
systemd user service works
SELinux behavior is documented or policy is supplied
Roland GO:KEYS is validated
at least one standard BLE-MIDI device is validated
midi-ble-rtctl scan/list/probe/connect/disconnect/forget is stable
pair/trust/connect policy works through profiles
reconnect works
ALSA ports are predictable
RX works
basic TX works
logs and stats are useful for troubleshooting
```

Conservative release split:

```text
1.0 = reliable daemon + CLI + profiles + reconnect + packaging
1.1 = AcquireNotify and realtime improvements
1.2 = polished bidirectional MIDI
```

## v1.1 — Low-jitter transport and realtime hygiene

Goal: reduce local daemon-side jitter after BLE delivery. This is optimization, not a prerequisite for basic usefulness.

Current fallback path:

```text
BLE notify -> bluetoothd -> D-Bus PropertiesChanged -> daemon -> ALSA
```

Possible target path:

```text
BLE notify -> bluetoothd -> AcquireNotify fd -> daemon RT thread -> ALSA
```

Tasks:

```text
measure current daemon-side latency/jitter
keep StartNotify as a stable fallback
design AcquireNotify fallback behavior
add read thread for notification fd when available
use preallocated buffers in hot paths
avoid malloc/free in hot paths
avoid printf/log formatting in hot paths
evaluate mlockall(MCL_CURRENT | MCL_FUTURE)
evaluate SCHED_FIFO only for MIDI dispatch thread
add latency/jitter counters
```

Exit criteria:

```text
StartNotify remains fallback
AcquireNotify works when BlueZ exposes it
metrics show lower daemon-side jitter
system remains stable without requiring RT privileges for normal use
```

Version target: `v1.1`.

## v1.2 — Polished bidirectional MIDI

Goal: make ALSA -> BLE-MIDI robust, not merely functional.

Tasks:

```text
stress-test aplaymidi
validate Note On/Off bursts
validate Program Change
validate Control Change
handle input-only devices cleanly
improve MTU-aware packetization
refine BLE-MIDI timestamp handling
expose TX counters and errors in stats
```

Exit criteria:

```text
DAW sends notes/control messages to the device reliably
input stream remains stable while TX is active
unsupported TX devices fail safely and visibly
statistics separate ALSA RX, ALSA TX, BLE RX and BLE TX clearly
```

Version target: `v1.2`.

## Historical mapping

The earlier roadmap used these phases:

```text
Phase 0  — consolidated proof
Phase 1  — bench daemon
Phase 2  — control CLI scan/list/probe
Phase 3  — profile-aware connect/disconnect/forget
Phase 4  — Agent1 authorization
Phase 5  — multi-keyboard daemon sessions
Phase 6  — reconnect and service mode
Phase 7  — device profiles and quirk library
Phase 8  — MIDI bidirectional path
Phase 9  — low-jitter transport: AcquireNotify and RT
Phase 10 — release 1.0
```

Post-`v0.6.3`, this map is interpreted as follows:

```text
Phases 0-1 are historical baseline.
Phase 2 is partially implemented and continues under v0.9 CLI work.
Phase 3 remains part of v0.9 CLI/profile policy work.
Phase 4 remains useful but is not required before generic-device validation.
Phase 5 becomes v0.8 multi-device runtime.
Phase 6 becomes packaging/service/reconnect validation toward v1.0.
Phase 7 becomes v0.7 generic BLE-MIDI validation.
Phase 8 becomes v1.2 polished bidirectional MIDI.
Phase 9 becomes v1.1 low-jitter/realtime work.
Phase 10 remains the public v1.0 target.
```
