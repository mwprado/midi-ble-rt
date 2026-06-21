# ALSA Sequencer interface

`midi-ble-rt` targets ALSA Sequencer as its primary and stable user-facing MIDI interface.

The project should not depend on PipeWire, JACK, a DAW API or any desktop-session MIDI API. Those systems may consume or display ALSA MIDI ports depending on the user's environment, but they are not part of the core contract.

## Contract

The daemon should expose BLE-MIDI devices as ALSA Sequencer ports:

```text
BLE-MIDI keyboard/controller
→ BlueZ GATT
→ midi-ble-rtd
→ ALSA Sequencer client/port
→ external MIDI consumer
```

The core deliverable is:

```text
stable ALSA client name
stable ALSA port name per device
valid ALSA MIDI events
usable with aconnect/aseqdump/DAWs/softsynths
```

## Non-goals

The daemon should not directly integrate with:

```text
PipeWire API
JACK API
DAW-specific APIs
WirePlumber policy
GUI routing tools
```

If PipeWire shows the ALSA MIDI port, that is a system-level consequence. It should not be required for correctness.

## Basic inspection

List ALSA sequencer clients and ports:

```bash
aconnect -l
```

Dump MIDI events from a port:

```bash
aseqdump -p "midi-ble-rt"
```

If multiple devices are active, the expected shape is:

```text
client 128: 'midi-ble-rt' [type=user]
    0 'Roland GO:KEYS BLE-MIDI'
    1 'Korg microKEY Air BLE-MIDI'
    2 'CME WIDI Master BLE-MIDI'
```

## Naming policy

Each device should have a predictable ALSA port name derived from the configured device id/profile, not from unstable BlueZ display names alone.

Example:

```ini
[device.roland-gokeys]
address = CB:81:F4:62:FF:07
profile = roland_gokeys
alsa_client_name = midi-ble-rt
alsa_port_name = Roland GO:KEYS BLE-MIDI
```

## PipeWire note

On modern Fedora/GNOME systems, PipeWire/WirePlumber may expose or bridge ALSA MIDI ports to graphical routing tools. That is acceptable, but optional. `midi-ble-rt` should remain correct even when tested only with:

```bash
aconnect -l
aseqdump
```

## Troubleshooting

If the daemon receives BLE packets but no ALSA port appears:

```bash
aconnect -l
ls -l /dev/snd/seq
ls -Z /dev/snd/seq
getenforce
```

If SELinux is enforcing and the port does not appear or ALSA open fails, check:

```bash
./scripts/selinux-diagnose.sh
sudo ausearch -m avc,user_avc -ts recent
```
