# midi-ble-rt command interface

`midi-ble-rt` has two operational layers:

```text
midi-ble-rtd   data-plane daemon: BLE-MIDI/GATT <-> ALSA Sequencer
midi-ble-rtctl control-plane CLI: scan, list, pair, trust, connect, forget and diagnostics
```

The CLI exists to make BlueZ faster and safer to operate for BLE-MIDI devices. It should not replace BlueZ. It automates the exact BlueZ/GATT sequence needed by MIDI instruments, controllers and adapters.

## Design goals

```text
1. list nearby BLE-MIDI candidates with address, name, alias, RSSI and UUID hints;
2. connect a selected BLE-MIDI device without using bluetoothctl manually;
3. apply profile/quirk policy per device model;
4. support more than one BLE-MIDI device at the same time;
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
  asks the daemon to attach/detach configured devices
```

## Device identity

Each device should have a stable user-facing id:

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
ALSA port name
connection policy
```

## Profiles and quirks

Profiles encode device policy, not only UUIDs.

```text
standard_ble_midi -> official BLE-MIDI service and I/O characteristic
roland_gokeys     -> official BLE-MIDI service plus observed Roland I/O alias
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

### list

```bash
midi-ble-rtctl list
midi-ble-rtctl list --midi-only
```

Purpose: list BlueZ-known devices and MIDI-capable candidates.

### probe

```bash
midi-ble-rtctl probe CB:81:F4:62:FF:07
midi-ble-rtctl probe --json CB:81:F4:62:FF:07
```

Purpose: connect temporarily if needed, wait for `ServicesResolved`, enumerate GATT and report service/characteristic candidates.

### connect

```bash
midi-ble-rtctl connect CB:81:F4:62:FF:07 --profile roland_gokeys
midi-ble-rtctl connect CB:81:F4:62:FF:07 --profile roland_gokeys --write-config
```

Purpose: apply the profile policy and prepare a BLE-MIDI device for the daemon.

For Roland GO:KEYS, this means:

```text
1. find Device1 by address;
2. pair if required by profile and not paired;
3. set Trusted=true;
4. connect Device1;
5. wait ServicesResolved;
6. validate BLE-MIDI GATT service;
7. select official I/O characteristic or Roland alias;
8. optionally write local config.
```

### daemon control

```bash
midi-ble-rtctl daemon-ping
midi-ble-rtctl daemon-status
midi-ble-rtctl daemon-list
midi-ble-rtctl daemon-connect DEVICE_ID_OR_ADDRESS
midi-ble-rtctl daemon-disconnect DEVICE_ID_OR_ADDRESS
midi-ble-rtctl daemon-recheck DEVICE_ID_OR_ADDRESS
```

Purpose: control or inspect an already running `midi-ble-rtd` instance.

### diagnostics

```bash
midi-ble-rtctl stats
midi-ble-rtctl top
midi-ble-rtctl latency
midi-ble-rtctl latency-top --interval 1000
```

Purpose: inspect runtime stats and latency diagnostic snapshots.

## Multiple device support

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
one MbSession per device
one GATT notify subscription per streaming device
one ALSA port per bound device
one profile/quirk instance per device
```

## BlueZ operations mapping

```text
scan        -> Adapter1.StartDiscovery / StopDiscovery
list        -> ObjectManager.GetManagedObjects
pair        -> Device1.Pair
trust       -> Properties.Set(Device1, Trusted=true)
untrust     -> Properties.Set(Device1, Trusted=false)
connect     -> Device1.Connect + GATT validation + optional config writing
disconnect  -> daemon stream stop + optional Device1.Disconnect
forget      -> Adapter1.RemoveDevice
probe       -> Device1.Connect + wait ServicesResolved + enumerate GATT
```

## Safety policy

`connect` may automate pair/trust only for a configured device or when the user passes `--profile` explicitly. For unknown devices, the CLI should show the proposed action and require confirmation unless `--yes` is used.

`forget` must require confirmation unless `--yes` is used.

`scan` and `list` should never modify BlueZ state except for temporary discovery.
