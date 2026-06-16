# midi-ble-rt

`midi-ble-rt` is a Linux BLE-MIDI/GATT to ALSA Sequencer bridge.

The first validated target is the Roland GO:KEYS family. The daemon does not depend on BlueZ's `profiles/midi` plugin. It uses BlueZ as a generic BLE/GATT transport, finds the BLE-MIDI service/characteristic directly, subscribes to notifications, decodes BLE-MIDI packets, and publishes an ALSA Sequencer source port for DAWs and softsynths.

## Current status

Validated proof of concept:

```text
Roland GO:KEYS MIDI
→ BLE/GATT
→ service 03b80e5a-ede8-4b33-a751-6ce34ec4c700
→ characteristic 00006bf3-0000-1000-8000-00805f9b34fb
→ StartNotify
→ midi-ble-rtd
→ ALSA Sequencer
→ aseqdump / DAW
```

Observed ALSA events:

```text
Note on   ch 0, note 96, velocity 26
Note off  ch 0, note 96, velocity 64
```

## Why this exists

Standard BLE-MIDI expects:

```text
Service:
03b80e5a-ede8-4b33-a751-6ce34ec4c700

MIDI I/O characteristic:
7772e5db-3868-4112-a1a9-f2669d106bf3
```

The tested Roland GO:KEYS exposes the standard service but uses this MIDI I/O characteristic instead:

```text
00006bf3-0000-1000-8000-00805f9b34fb
```

with:

```text
read
write-without-response
write
notify
MTU 96
```

`midi-ble-rt` treats this as a Roland alias/quirk inside the standard BLE-MIDI service.

## Build on Fedora

```bash
sudo dnf install gcc cmake glib2-devel alsa-lib-devel bluez bluez-tools alsa-utils
cmake -S . -B build
cmake --build build
```

## Control CLI

Phase 1 adds `midi-ble-rtctl`, a BlueZ-oriented inspection tool for BLE-MIDI devices. It does not create ALSA ports; that remains the job of `midi-ble-rtd`.

Useful commands:

```bash
./build/midi-ble-rtctl list
./build/midi-ble-rtctl list --midi-only
./build/midi-ble-rtctl scan --timeout 10 --midi-only
./build/midi-ble-rtctl info CB:81:F4:62:FF:07
./build/midi-ble-rtctl probe CB:81:F4:62:FF:07
```

`scan` starts BlueZ discovery and then prints known devices with address, RSSI, name, alias, paired/trusted/connected state, BLE-MIDI UUID hints and profile guesses.

`probe` may call `Device1.Connect()` temporarily to let BlueZ resolve GATT services. It then enumerates the BLE-MIDI service and scores candidate MIDI I/O characteristics, including the Roland GO:KEYS `00006bf3...` alias.

## Run

Create a config file:

```bash
mkdir -p ~/.config/midi-ble-rt
cp config/roland-gokeys.ini.example ~/.config/midi-ble-rt/roland-gokeys.ini
```

Edit the address if needed, then run:

```bash
./build/midi-ble-rtd --config ~/.config/midi-ble-rt/roland-gokeys.ini
```

Check the ALSA port:

```bash
aconnect -l
aseqdump -p "midi-ble-rt"
```

## Operational rule for Roland GO:KEYS

Connect MIDI first, then Audio/A2DP if needed.

When the GO:KEYS is connected as Audio, it behaves as a Bluetooth speaker/receptor. That is not the MIDI path. The daemon validates the target by GATT, not by the BlueZ `Name` or `Alias`.

## SELinux

SELinux can block the ALSA Sequencer side of the integration. Do not solve this by globally disabling SELinux. See:

```text
docs/SELINUX.md
selinux/bluez_midi_alsa.te.example
scripts/selinux-diagnose.sh
```

Quick diagnostic:

```bash
./scripts/selinux-diagnose.sh
```

If the native BlueZ MIDI profile fails only with SELinux enforcing, capture the `bluetoothd` AVC and generate a narrow local module from the real denial. If `midi-ble-rtd` is installed as a confined system service, generate a daemon-specific policy from its own AVCs.

## Roadmap

- [x] GATT discovery by service UUID
- [x] Roland GO:KEYS `00006bf3...` characteristic alias
- [x] ALSA Sequencer source port
- [x] BLE-MIDI note decoding proof
- [x] initial `midi-ble-rtctl list/scan/info/probe`
- [ ] profile-aware `midi-ble-rtctl connect/disconnect/forget`
- [ ] BlueZ Agent1 integration for automatic pair/authorize
- [ ] automatic reconnect loop
- [ ] `AcquireNotify()` fast path
- [ ] bidirectional MIDI write path
- [ ] RT thread, `mlockall()`, jitter metrics
- [ ] profiles for Korg/Yamaha/CME/Kawai and standard BLE-MIDI devices

## Related BlueZ patch idea

A minimal BlueZ compatibility patch should accept `00006bf3-0000-1000-8000-00805f9b34fb` as a Roland MIDI I/O alias when it is found inside the standard BLE-MIDI service and exposes `notify` plus `write-without-response`.
