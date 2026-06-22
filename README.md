# midi-ble-rt

`midi-ble-rt` is a Linux BLE-MIDI/GATT to ALSA Sequencer bridge.

The first validated target is the Roland GO:KEYS family. The daemon does not depend on BlueZ's `profiles/midi` plugin. It uses BlueZ as a generic BLE/GATT transport, finds the BLE-MIDI service/characteristic directly, subscribes to notifications, decodes BLE-MIDI packets, publishes an ALSA Sequencer port, and can write short MIDI messages back to the keyboard through GATT `WriteValue`.

The primary interface is ALSA Sequencer. PipeWire/JACK/DAWs may consume or display the ALSA MIDI port, but they are not dependencies of the project.

## Current status

Validated receive path:

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

Initial transmit path:

```text
aplaymidi / DAW / ALSA Sequencer client
→ midi-ble-rtd duplex ALSA port
→ BLE-MIDI packet encoder
→ GATT WriteValue
→ Roland GO:KEYS
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

`midi-ble-rtctl` is the BlueZ control-plane tool. It helps discover, inspect, prepare and configure BLE-MIDI devices without manually driving `bluetoothctl` for every step.

The examples below use the installed command names. When running directly from a local build tree, prefix the commands with the build path, for example `./build/midi-ble-rtctl`.

Help commands:

```bash
midi-ble-rtctl --help
midi-ble-rtctl help
midi-ble-rtctl help connect
midi-ble-rtctl help configure
midi-ble-rtctl connect --help
midi-ble-rtctl configure --help
```

Inspection commands:

```bash
midi-ble-rtctl list
midi-ble-rtctl list --midi-only
midi-ble-rtctl scan --timeout 10 --midi-only
midi-ble-rtctl info CB:81:F4:62:FF:07
midi-ble-rtctl probe CB:81:F4:62:FF:07
```

BlueZ control commands:

```bash
midi-ble-rtctl pair CB:81:F4:62:FF:07
midi-ble-rtctl trust CB:81:F4:62:FF:07
midi-ble-rtctl untrust CB:81:F4:62:FF:07
midi-ble-rtctl connect CB:81:F4:62:FF:07 --profile roland_gokeys
midi-ble-rtctl disconnect CB:81:F4:62:FF:07
midi-ble-rtctl forget CB:81:F4:62:FF:07 --yes
```

Configuration commands:

```bash
midi-ble-rtctl configure CB:81:F4:62:FF:07 --profile roland_gokeys
midi-ble-rtctl configure CB:81:F4:62:FF:07 --profile roland_gokeys --print
midi-ble-rtctl configure CB:81:F4:62:FF:07 --profile roland_gokeys --force
midi-ble-rtctl connect CB:81:F4:62:FF:07 --profile roland_gokeys --write-config
```

For `--profile roland_gokeys`, `connect` applies the current Roland policy:

```text
Pair if needed
Trusted=true
Device1.Connect()
wait ServicesResolved=true
validate BLE-MIDI service
accept official MIDI I/O UUID or Roland 00006bf3... alias
```

`connect --write-config` writes the local config after a successful BlueZ connection/GATT validation. The default GO:KEYS config path is:

```text
~/.config/midi-ble-rt/roland-gokeys.ini
```

`connect` does not yet start the streaming daemon automatically. For now, the data plane is still started with `midi-ble-rtd --config <file>`.

`scan` starts BlueZ discovery and then prints known devices with address, RSSI, name, alias, paired/trusted/connected state, BLE-MIDI UUID hints and profile guesses.

`probe` may call `Device1.Connect()` temporarily to let BlueZ resolve GATT services. It then enumerates the BLE-MIDI service and scores candidate MIDI I/O characteristics, including the Roland GO:KEYS `00006bf3...` alias.

## Run data plane

Create or update the config file automatically:

```bash
midi-ble-rtctl configure CB:81:F4:62:FF:07 --profile roland_gokeys --force
```

Then run:

```bash
midi-ble-rtd --config ~/.config/midi-ble-rt/roland-gokeys.ini
```

Check the ALSA port:

```bash
aconnect -l
aseqdump -p CLIENT:PORT
```

Use the numeric `CLIENT:PORT` shown by `aconnect -l`, for example `128:0`.

## Receive MIDI from GO:KEYS

```bash
aseqdump -p 128:0
```

or record a MIDI file:

```bash
arecordmidi -p 128:0 gokeys-input.mid
```

## Send MIDI to GO:KEYS

The daemon creates a duplex ALSA Sequencer port. After the daemon is running, send a MIDI file into the same `CLIENT:PORT`:

```bash
aplaymidi -p 128:0 test.mid
```

For a minimal smoke test, use the repository test MIDI file:

```bash
aplaymidi -p 128:0 examples/midi/test-note.mid
```

With debug enabled, the daemon prints outgoing MIDI bytes and BLE packets.

Transmit can be disabled in the config:

```ini
[midi]
enable_tx = no
```

## Hardware-free tests

Hardware-free tests are documented in:

```text
docs/TESTING.md
```

Validate ALSA MIDI write/read without GO:KEYS:

```bash
scripts/test-alsa-loopback.sh
```

Validate the example MIDI file through FluidSynth without GO:KEYS:

```bash
scripts/test-fluidsynth-smoke.sh
```

The FluidSynth test requires an existing FluidSynth ALSA Sequencer input port. If the port is not auto-detected:

```bash
FLUIDSYNTH_PORT=128:0 scripts/test-fluidsynth-smoke.sh
```

These tests validate ALSA and MIDI fixtures. They do not validate BlueZ GATT behavior.

## Operational rule for Roland GO:KEYS

Connect MIDI first, then Audio/A2DP if needed.

When the GO:KEYS is connected as Audio, it behaves as a Bluetooth speaker/receptor. That is not the MIDI path. The daemon validates the target by GATT, not by the BlueZ `Name` or `Alias`.

## ALSA, not PipeWire

The project creates ALSA Sequencer ports. See:

```text
docs/ALSA.md
```

PipeWire may expose ALSA MIDI ports in some environments, but PipeWire is not part of the core dependency chain.

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
- [x] ALSA Sequencer duplex port
- [x] BLE-MIDI note decoding proof
- [x] initial `midi-ble-rtctl list/scan/info/probe`
- [x] basic `midi-ble-rtctl pair/trust/connect/disconnect/forget`
- [x] detailed `midi-ble-rtctl --help` and per-command help
- [x] profile-aware local config for GO:KEYS
- [x] initial ALSA -> BLE-MIDI write path
- [x] hardware-free ALSA/FluidSynth smoke tests
- [ ] multiple device ids and config directory loading
- [ ] daemon control from `midi-ble-rtctl connect`
- [ ] BlueZ Agent1 integration for automatic pair/authorize
- [ ] automatic reconnect loop
- [ ] `AcquireNotify()` fast path
- [ ] robust bidirectional MIDI write path for long SysEx/segmentation edge cases
- [ ] RT thread, `mlockall()`, jitter metrics
- [ ] profiles for Korg/Yamaha/CME/Kawai and standard BLE-MIDI devices

## Related BlueZ patch idea

A minimal BlueZ compatibility patch should accept `00006bf3-0000-1000-8000-00805f9b34fb` as a Roland MIDI I/O alias when it is found inside the standard BLE-MIDI service and exposes `notify` plus `write-without-response`.
