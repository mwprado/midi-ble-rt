# Project roadmap

This roadmap organizes `midi-ble-rt` as a practical Linux BLE-MIDI stack with a daemon, a control CLI, device profiles and optional realtime transport improvements.

The project should evolve in this order:

```text
reliable daemon first
control CLI second
multiple keyboards third
low-jitter/realtime path after the basics are stable
```

## Phase 0 — Consolidated proof

Status: current repository baseline.

Goal: prove the technical chain.

```text
Roland GO:KEYS
→ BlueZ generic GATT
→ BLE-MIDI service
→ Roland 00006bf3... characteristic alias
→ StartNotify
→ BLE-MIDI decoder
→ ALSA Sequencer
→ aseqdump / DAW
```

Done:

```text
config .ini
BlueZ system D-Bus connection
Device1 selection
trust
connect
ServicesResolved wait
GATT service discovery
characteristic scoring
StartNotify
ALSA Sequencer source port
minimal BLE-MIDI decoder
SELinux notes
```

Exit criteria:

```bash
cmake -S . -B build
cmake --build build
./build/midi-ble-rtd --config config/roland-gokeys.ini.example
aconnect -l
aseqdump -p "midi-ble-rt"
```

Version target: `v0.0` / proof of concept.

## Phase 1 — Bench daemon

Goal: make `midi-ble-rtd` reliable for manual use.

Tasks:

```text
split source into modules
validate require_notify and require_write_without_response
improve error messages
improve parser diagnostics
make logs consistent
keep ALSA port stable during runtime
handle clean shutdown
```

Suggested structure:

```text
src/
  main.c
  config.c
  bluez_device.c
  gatt_discovery.c
  alsa_seq.c
  ble_midi_decode.c
  log.c
```

Exit criteria:

```text
builds cleanly
runs without GNOME
creates one ALSA port
delivers Note On/Off reliably
failure messages explain the next action
```

Version target: `v0.1`.

## Phase 2 — Control CLI: scan/list/probe

Goal: add `midi-ble-rtctl` as a fast BlueZ-oriented MIDI tool.

First commands:

```bash
midi-ble-rtctl scan
midi-ble-rtctl list
midi-ble-rtctl info DEVICE
midi-ble-rtctl probe DEVICE
```

Behavior:

```text
scan nearby devices
show address/name/alias/RSSI/UUID hints
show known BlueZ devices
connect temporarily for GATT inspection when probing
infer possible profile/quirk
print selected BLE-MIDI service and characteristic
```

Exit criteria:

```text
user can discover GO:KEYS address without bluetoothctl
user can see candidate profile
user can confirm 00006bf3... from the CLI
```

Version target: `v0.2`.

## Phase 3 — Profile-aware connect/disconnect/forget

Goal: make `connect` encode device quirks and BlueZ setup policy.

Commands:

```bash
midi-ble-rtctl connect DEVICE
midi-ble-rtctl disconnect DEVICE
midi-ble-rtctl forget DEVICE
midi-ble-rtctl pair DEVICE
midi-ble-rtctl trust DEVICE
midi-ble-rtctl untrust DEVICE
```

For Roland GO:KEYS, `connect` should do:

```text
find Device1
Pair if required and not paired
Set Trusted=true
Device1.Connect
Wait ServicesResolved=true
Validate BLE-MIDI service
Accept official I/O characteristic or Roland 00006bf3 alias
Start daemon stream or spawn daemon instance
Create/keep ALSA port
```

Quirk policy should decide whether `pair`, `trust` and MIDI-first connection are mandatory.

Exit criteria:

```text
fresh BlueZ device can be prepared without bluetoothctl
connect go-keys applies pair/trust/connect automatically
forget removes device from BlueZ and local config
```

Version target: `v0.3`.

## Phase 4 — Agent1 authorization

Goal: remove dependency on external `bluetoothctl agent`.

Implement:

```text
org.bluez.Agent1
AgentManager1.RegisterAgent
AgentManager1.RequestDefaultAgent
RequestConfirmation
RequestAuthorization
AuthorizeService
Release
Cancel
```

Initial policy:

```text
NoInputNoOutput
accept only configured device/profile
reject unknown devices unless explicitly confirmed
```

Exit criteria:

```text
delete the device from BlueZ
run midi-ble-rtctl connect roland-gokeys
pair/trust/connect/StartNotify succeeds without bluetoothctl
```

Version target: `v0.4`.

## Phase 5 — Multi-keyboard daemon sessions

Goal: support more than one BLE-MIDI keyboard/controller at the same time.

Model:

```text
one daemon process
multiple DeviceSession objects
one GATT notify subscription per device
one ALSA port per device
one profile/quirk per device
```

Expected ALSA shape:

```text
client 128: 'midi-ble-rt' [type=user]
    0 'Roland GO:KEYS BLE-MIDI'
    1 'Korg microKEY Air BLE-MIDI'
    2 'CME WIDI Master BLE-MIDI'
```

Configuration:

```ini
[device.roland-gokeys]
address = CB:81:F4:62:FF:07
profile = roland_gokeys
autoconnect = yes
alsa_port_name = Roland GO:KEYS BLE-MIDI

[device.korg-air]
address = AA:BB:CC:DD:EE:FF
profile = standard_ble_midi
autoconnect = no
alsa_port_name = Korg microKEY Air BLE-MIDI
```

Exit criteria:

```text
two configured keyboards produce two ALSA ports
one device can disconnect/reconnect without killing the other
CLI status shows each session independently
```

Version target: `v0.5`.

## Phase 6 — Reconnect and service mode

Goal: make `midi-ble-rtd` a real background service.

Tasks:

```text
observe Device1.Connected changes
observe InterfacesRemoved
detect notify loss
backoff reconnect
reuse ALSA port or recreate predictably
systemd unit hardening
journalctl-friendly logs
SELinux enforcing validation
```

Policy:

```ini
[device.roland-gokeys]
autoconnect = yes
reconnect_initial_delay_ms = 500
reconnect_max_delay_ms = 10000
```

Exit criteria:

```text
start service
turn keyboard off
service keeps running
turn keyboard on
stream returns
ALSA identity remains usable
```

Version target: `v0.6`.

## Phase 7 — Device profiles and quirk library

Goal: make the project useful beyond Roland.

Profiles:

```text
standard_ble_midi
roland_gokeys
korg_air
yamaha_ble_midi
cme_widi
kawai_ble_midi
```

Each profile should define:

```text
service_uuid
io_uuid
aliases
required flags
pair policy
trust policy
connect order
known warnings
parser quirks if any
```

Exit criteria:

```text
standard BLE-MIDI profile works
Roland GO:KEYS profile works
profile can be selected by config or CLI
new profile can be added without editing core daemon code
```

Version target: `v0.7`.

## Phase 8 — MIDI bidirectional path

Goal: support ALSA -> BLE-MIDI writes.

Tasks:

```text
make ALSA port duplex
read ALSA input events
encode MIDI bytes into BLE-MIDI packets
WriteValue or AcquireWrite
MTU-aware packetization
timestamp handling
```

Exit criteria:

```text
DAW sends Program Change / notes to keyboard
keyboard responds
input stream remains stable
```

Version target: `v0.8`.

## Phase 9 — Low-jitter transport: AcquireNotify and RT

Goal: reduce local jitter after BLE delivery.

Current path:

```text
BLE notify -> bluetoothd -> D-Bus PropertiesChanged -> daemon -> ALSA
```

Target path:

```text
BLE notify -> bluetoothd -> AcquireNotify fd -> daemon RT thread -> ALSA
```

Tasks:

```text
AcquireNotify fallback design
read thread for notification fd
preallocated buffers
no malloc/free in hot path
no printf in hot path
mlockall(MCL_CURRENT | MCL_FUTURE)
SCHED_FIFO only for MIDI dispatch thread
latency/jitter counters
```

Exit criteria:

```text
StartNotify remains fallback
AcquireNotify works when BlueZ exposes it
metrics show lower daemon-side jitter
system remains stable
```

Version target: `v0.9`.

## Phase 10 — Release 1.0

Goal: stable public release.

Minimum requirements:

```text
GO:KEYS validated
one standard BLE-MIDI device validated
midi-ble-rtctl scan/list/probe/connect/disconnect/forget stable
pair/trust/connect policy by profile
Agent1 works
multi-device sessions work
reconnect works
SELinux enforcing documented or policy supplied
systemd service works
ALSA ports predictable
logs are useful
```

Conservative release split:

```text
1.0 = reliable daemon + CLI + profiles + reconnect
1.1 = AcquireNotify and realtime improvements
1.2 = bidirectional MIDI polished
```
