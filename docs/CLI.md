# midi-ble-rt command interface

`midi-ble-rt` should be split into two operational layers:

```text
midi-ble-rtd   data-plane daemon: BLE-MIDI/GATT -> ALSA Sequencer
midi-ble-rtctl control-plane CLI: scan, list, pair, trust, connect, forget, inspect quirks
```

The CLI exists to make BlueZ faster and safer to operate for BLE-MIDI devices. It should not replace BlueZ. It should automate the exact BlueZ/GATT sequence needed by MIDI controllers and keyboards.

## Design goals

```text
1. list nearby BLE-MIDI candidates with address, name, alias, RSSI and UUID hints;
2. connect a selected keyboard without using bluetoothctl manually;
3. apply profile/quirk policy per device model;
4. support more than one keyboard at the same time;
5. keep ALSA port names predictable;
6. make failure modes explicit: pairing required, authorization denied, GATT unresolved, wrong Audio/MIDI order;
7. provide commands that map directly to BlueZ operations when possible.
```

## Data plane vs control plane

```text
midi-ble-rtd
  owns active BLE-MIDI streams
  subscribes to GATT notifications
  creates ALSA Sequencer ports
  reconnects active devices

midi-ble-rtctl
  discovers devices
  pairs/trusts/connects/disconnects/forgets devices
  selects profiles and writes config
  can ask the daemon to attach/detach devices in a later phase
```

Initial implementation may let `midi-ble-rtctl connect` perform BlueZ setup and then spawn or instruct `midi-ble-rtd` for the actual MIDI stream. Later, the daemon should expose a local control socket or D-Bus interface.

## Device identity

Each keyboard should have a stable user-facing id:

```text
roland-gokeys
korg-microkey-air
yamaha-md-bt01
cme-widi-master
```

The id maps to:

```text
Bluetooth address
BlueZ object path
profile/quirk
ALSA client/port names
connection policy
```

Example config model:

```ini
[device.roland-gokeys]
address = CB:81:F4:62:FF:07
name = GO:KEYS MIDI 1
profile = roland_gokeys
autoconnect = yes
alsa_client_name = midi-ble-rt
alsa_port_name = Roland GO:KEYS BLE-MIDI

[device.korg-air]
address = AA:BB:CC:DD:EE:FF
name = microKEY Air
profile = standard_ble_midi
autoconnect = no
alsa_client_name = midi-ble-rt
alsa_port_name = Korg microKEY Air BLE-MIDI
```

## Profiles and quirks

Profiles should encode policy, not only UUIDs.

```ini
[profile.standard_ble_midi]
service_uuid = 03b80e5a-ede8-4b33-a751-6ce34ec4c700
io_uuid = 7772e5db-3868-4112-a1a9-f2669d106bf3
require_notify = yes
require_write_without_response = yes
pair_before_notify = auto
trust_after_pair = yes
connect_order = midi-first

[profile.roland_gokeys]
service_uuid = 03b80e5a-ede8-4b33-a751-6ce34ec4c700
io_uuid = 7772e5db-3868-4112-a1a9-f2669d106bf3
io_uuid_alias = 00006bf3-0000-1000-8000-00805f9b34fb
require_notify = yes
require_write_without_response = yes
pair_before_notify = yes
trust_after_pair = yes
connect_order = midi-first
notes = Connect MIDI before Audio/A2DP.
```

For Roland GO:KEYS, `connect` should perform the policy sequence directly:

```text
Pair if needed
Set Trusted=true
Connect Device1
Wait ServicesResolved=true
Find BLE-MIDI service
Prefer official I/O UUID; accept Roland 00006bf3 alias
StartNotify or ask daemon to start stream
Create/keep ALSA port
```

## Commands

### scan

```bash
midi-ble-rtctl scan
midi-ble-rtctl scan --timeout 10
midi-ble-rtctl scan --midi-only
midi-ble-rtctl scan --json
```

Purpose: start BlueZ discovery and show likely MIDI devices.

Output example:

```text
ADDRESS            RSSI  NAME             UUID-HINT      PROFILE-GUESS
CB:81:F4:62:FF:07  -53   GO:KEYS MIDI 1   ble-midi      roland_gokeys?
AA:BB:CC:DD:EE:FF  -61   microKEY Air     ble-midi      standard_ble_midi?
```

The scan command should use advertisement UUIDs and names only. Full GATT confirmation requires connect/probe.

### list

```bash
midi-ble-rtctl list
midi-ble-rtctl list --known
midi-ble-rtctl list --connected
midi-ble-rtctl list --active
midi-ble-rtctl list --json
```

Purpose: list BlueZ-known devices and daemon-active devices.

Output example:

```text
ID              ADDRESS            NAME             PAIRED TRUSTED CONNECTED ACTIVE PROFILE
roland-gokeys   CB:81:F4:62:FF:07  GO:KEYS MIDI 1   yes    yes     yes       yes    roland_gokeys
korg-air        AA:BB:CC:DD:EE:FF  microKEY Air     no     no      no        no     standard_ble_midi
```

### probe

```bash
midi-ble-rtctl probe CB:81:F4:62:FF:07
midi-ble-rtctl probe roland-gokeys
midi-ble-rtctl probe --json CB:81:F4:62:FF:07
```

Purpose: connect temporarily if needed, wait for `ServicesResolved`, enumerate GATT and report service/characteristic candidates.

Output example:

```text
Device: GO:KEYS MIDI 1 / CB:81:F4:62:FF:07
Service: 03b80e5a-ede8-4b33-a751-6ce34ec4c700
Characteristic: 00006bf3-0000-1000-8000-00805f9b34fb
Flags: read write-without-response write notify
Profile guess: roland_gokeys
Status: usable
```

### connect

```bash
midi-ble-rtctl connect roland-gokeys
midi-ble-rtctl connect CB:81:F4:62:FF:07 --profile roland_gokeys
midi-ble-rtctl connect --all
midi-ble-rtctl connect roland-gokeys --no-stream
```

Purpose: apply the profile policy and make the ALSA MIDI port ready.

For Roland GO:KEYS, this means:

```text
1. find Device1 by address or name;
2. pair if required by profile and not paired;
3. set Trusted=true;
4. connect Device1;
5. wait ServicesResolved;
6. validate BLE-MIDI GATT service;
7. select official I/O characteristic or Roland alias;
8. start daemon stream;
9. expose ALSA port.
```

`--no-stream` should stop after BlueZ setup and GATT validation.

### disconnect

```bash
midi-ble-rtctl disconnect roland-gokeys
midi-ble-rtctl disconnect CB:81:F4:62:FF:07
midi-ble-rtctl disconnect --all
```

Purpose: stop the daemon stream and optionally disconnect BlueZ.

Policy:

```text
stop GATT notify/stream
remove or mark ALSA port inactive
call Device1.Disconnect only when requested or configured
keep pairing/trust unless forget is requested
```

Optional flags:

```bash
midi-ble-rtctl disconnect roland-gokeys --bluez
midi-ble-rtctl disconnect roland-gokeys --keep-bluez
```

### forget

```bash
midi-ble-rtctl forget roland-gokeys
midi-ble-rtctl forget CB:81:F4:62:FF:07
```

Purpose: remove the device from BlueZ and local config.

Policy:

```text
stop active stream
disconnect if connected
call Adapter1.RemoveDevice(Device1)
remove local device entry, unless --keep-config is used
```

Optional flags:

```bash
midi-ble-rtctl forget roland-gokeys --keep-config
midi-ble-rtctl forget roland-gokeys --yes
```

### trust / untrust

```bash
midi-ble-rtctl trust roland-gokeys
midi-ble-rtctl untrust roland-gokeys
```

Purpose: direct wrappers over BlueZ `Device1.Trusted`.

### pair

```bash
midi-ble-rtctl pair roland-gokeys
midi-ble-rtctl pair CB:81:F4:62:FF:07 --profile roland_gokeys
```

Purpose: explicit pairing, normally folded into `connect` for profiles that require it.

### info

```bash
midi-ble-rtctl info roland-gokeys
midi-ble-rtctl info CB:81:F4:62:FF:07 --gatt
midi-ble-rtctl info roland-gokeys --json
```

Purpose: show everything relevant for debugging.

```text
BlueZ path
Address
Name/Alias
RSSI
Paired/Bonded/Trusted/Connected
ServicesResolved
advertised UUIDs
GATT services/chars/flags
selected profile
selected MIDI characteristic
ALSA port name
SELinux hints if /dev/snd/seq fails
```

### start / stop / status

```bash
midi-ble-rtctl start roland-gokeys
midi-ble-rtctl stop roland-gokeys
midi-ble-rtctl status
midi-ble-rtctl status --json
```

Purpose: control the daemon stream without changing BlueZ pairing/trust state.

## Multiple keyboard support

Multiple devices should create multiple ALSA ports.

Example:

```text
client 128: 'midi-ble-rt' [type=user]
    0 'Roland GO:KEYS BLE-MIDI'
    1 'Korg microKEY Air BLE-MIDI'
    2 'CME WIDI Master BLE-MIDI'
```

Internal model:

```text
one daemon process
one DeviceSession per keyboard
one GATT notify subscription per keyboard
one ALSA port per keyboard
one profile/quirk instance per keyboard
```

The first robust implementation may use one daemon process with multiple sessions. A simpler transitional version may spawn one `midi-ble-rtd --device ...` process per keyboard, but that should not be the final architecture.

## BlueZ operations mapping

```text
scan        -> Adapter1.StartDiscovery / StopDiscovery
list        -> ObjectManager.GetManagedObjects
pair        -> Device1.Pair
trust       -> Properties.Set(Device1, Trusted=true)
untrust     -> Properties.Set(Device1, Trusted=false)
connect     -> Device1.Connect + GATT validation + daemon stream start
disconnect  -> daemon stream stop + optional Device1.Disconnect
forget      -> Adapter1.RemoveDevice
probe       -> Device1.Connect + wait ServicesResolved + enumerate GATT
```

## Implementation phases for the CLI

```text
CLI-0: read-only list/info from BlueZ cache
CLI-1: scan/list/probe
CLI-2: pair/trust/connect/disconnect/forget wrappers
CLI-3: profile-aware connect policy
CLI-4: write local config entries
CLI-5: control daemon sessions
CLI-6: multi-keyboard daemon control
CLI-7: JSON output and shell-completion
```

## Safety policy

`connect` may automate pair/trust only for a configured device or when the user passes `--profile` explicitly. For unknown devices, the CLI should show the proposed action and require confirmation unless `--yes` is used.

`forget` must require confirmation unless `--yes` is used.

`scan` and `list` should never modify BlueZ state except for temporary discovery.
