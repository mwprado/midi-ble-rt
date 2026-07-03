# Generic BLE-MIDI device tutorial

This tutorial shows how to use `midi-ble-rt` with a generic BLE-MIDI instrument,
controller or MIDI adapter using the standard directory-based configuration.

The device does not need to be a keyboard. It can be any standards-compliant
BLE-MIDI device, for example:

- keyboard;
- pad controller;
- wind controller;
- electronic drum module;
- synthesizer;
- sound module;
- MIDI surface controller;
- BLE-MIDI DIN adapter.

The requirement is technical: the device must expose the standard BLE-MIDI GATT
service and I/O characteristic.

```text
Service UUID:        03b80e5a-ede8-4b33-a751-6ce34ec4c700
I/O characteristic:  7772e5db-3868-4112-a1a9-f2669d106bf3
```

Bluetooth Classic audio, USB-MIDI and vendor-specific wireless protocols are
outside this generic path.

## Data path

```text
BLE-MIDI device <-> BlueZ/GATT <-> midi-ble-rtd <-> ALSA Sequencer <-> MIDI app
```

`midi-ble-rtd` does not require PipeWire for the core BLE-MIDI bridge. PipeWire,
DAWs and regular Linux MIDI tools can consume the ALSA Sequencer port exported by
the daemon.

## 1. Install the package

On regular Fedora:

```bash
sudo dnf install 'dnf-command(copr)'
sudo dnf copr enable mwprado/cangaceiro
sudo dnf install midi-ble-rt
```

Installed commands:

```text
midi-ble-rtd
midi-ble-rtctl
```

Installed example configs:

```text
/usr/share/midi-ble-rt/config/
```

## 2. Put the device in BLE-MIDI mode

Turn on the instrument/controller and enable its Bluetooth MIDI mode according to
its manual.

Important distinctions:

- use the MIDI/BLE mode, not Bluetooth Audio/A2DP;
- do not rely only on the displayed name;
- validate the device through GATT service discovery;
- use the Bluetooth address as the stable technical identity.

Some devices advertise separate entries for audio and MIDI. The MIDI target is
the one exposing the BLE-MIDI GATT service.

## 3. Scan for BLE-MIDI candidates

```bash
midi-ble-rtctl scan --timeout 10 --midi-only
```

Also list devices already known by BlueZ:

```bash
midi-ble-rtctl list --midi-only
```

Write down the Bluetooth address shown by the CLI, for example:

```text
AA:BB:CC:DD:EE:FF
```

Use the address, not the name, in the configuration.

## 4. Inspect and validate the GATT service

Probe the device:

```bash
midi-ble-rtctl probe AA:BB:CC:DD:EE:FF
```

The expected generic result is:

```text
Service:        03b80e5a-ede8-4b33-a751-6ce34ec4c700
Characteristic: 7772e5db-3868-4112-a1a9-f2669d106bf3
Flags:          notify write-without-response
Status:         usable
```

If the service is missing, the device is not visible as a standard BLE-MIDI
device to BlueZ. If the characteristic exists but does not support notification,
receive will not work. If write support is missing or unstable, transmit may need
to be disabled.

## 5. Copy the standard directory configuration

For an installed package:

```bash
mkdir -p ~/.config/midi-ble-rt/devices.d
cp /usr/share/midi-ble-rt/config/daemon.ini.example \
   ~/.config/midi-ble-rt/daemon.ini
cp /usr/share/midi-ble-rt/config/devices.d/standard-ble-midi.ini.example \
   ~/.config/midi-ble-rt/devices.d/standard-ble-midi.ini
```

For a local build tree:

```bash
mkdir -p ~/.config/midi-ble-rt/devices.d
cp config/daemon.ini.example ~/.config/midi-ble-rt/daemon.ini
cp config/devices.d/standard-ble-midi.ini.example \
   ~/.config/midi-ble-rt/devices.d/standard-ble-midi.ini
```

Edit the device file:

```bash
$EDITOR ~/.config/midi-ble-rt/devices.d/standard-ble-midi.ini
```

Replace the address and enable startup connection:

```ini
[device]
id = generic-example
enabled = yes
address = AA:BB:CC:DD:EE:FF
name = BLE-MIDI
profile = standard_ble_midi
connect_on_start = yes
alsa_port_name = BLE-MIDI
```

Keep the standard GATT section in `daemon.ini` unchanged for the first test:

```ini
[gatt]
service_uuid = 03b80e5a-ede8-4b33-a751-6ce34ec4c700
io_uuid = 7772e5db-3868-4112-a1a9-f2669d106bf3
io_uuid_alias =
require_notify = yes
require_write_without_response = yes
```

The standard config intentionally leaves `io_uuid_alias` empty. Vendor-specific
aliases belong in device-specific configs or profiles, not in the generic path.

## 6. Pair or trust only if the device requires it

The standard policy starts conservative:

```ini
[policy]
pair = no
trust = yes
reconnect_on_link_loss = yes
```

If the device works without explicit pairing, leave pairing disabled.

If the device requires pairing, run:

```bash
midi-ble-rtctl pair AA:BB:CC:DD:EE:FF
midi-ble-rtctl trust AA:BB:CC:DD:EE:FF
```

Then probe again:

```bash
midi-ble-rtctl probe AA:BB:CC:DD:EE:FF
```

## 7. Start the daemon with the config directory

```bash
midi-ble-rtd --config ~/.config/midi-ble-rt
```

Expected startup marker:

```text
midi-ble-rtd
Runtime: config-directory multi-session
```

The daemon should connect through BlueZ/GATT, subscribe to BLE-MIDI
notifications, create an ALSA Sequencer port and enter the streaming state.

## 8. Find the ALSA Sequencer port

In another terminal:

```bash
aconnect -l
```

Look for the `midi-ble-rt` client and the configured port name:

```ini
[device]
alsa_port_name = BLE-MIDI
```

Use the numeric `CLIENT:PORT` shown by `aconnect -l`, for example:

```text
128:0
```

## 9. Receive MIDI from the device

Monitor incoming MIDI events:

```bash
aseqdump -p 128:0
```

Play notes, move pads, turn knobs, hit drum pads or trigger any MIDI control on
the device. The terminal should show MIDI events.

Record a MIDI file:

```bash
arecordmidi -p 128:0 generic-input.mid
```

Stop recording with `Ctrl+C`.

## 10. Send MIDI to the device

If the device receives MIDI over BLE-MIDI, send a MIDI file to the same ALSA port:

```bash
aplaymidi -p 128:0 test.mid
```

For a local repository checkout, a minimal smoke test file is available:

```bash
aplaymidi -p 128:0 examples/midi/test-note.mid
```

If the device is receive-only from the Linux side, or if TX is unstable, disable
transmit:

```ini
[midi]
enable_tx = no
```

Then restart the daemon.

## 11. Check reconnect behavior

The standard policy enables reconnect:

```ini
[policy]
reconnect_on_link_loss = yes
```

On recoverable BlueZ/GATT failures, the session should move to `RECONNECTING`
instead of exiting. Current operational timings are:

```text
Device1.Connect(): 30 s
ServicesResolved: 15 s
StartNotify:      15 s
WriteValue:        5 s
health check:      1 s
reconnect retry:  10 s
```

The session stats file is:

```text
/run/user/$UID/midi-ble-rt/stats.tsv
```

Expected recovery states:

```text
CONNECTING -> RECONNECTING -> STREAMING
```

## 12. Optional latency diagnostics

Enable diagnostics in `daemon.ini`:

```ini
[stats]
enabled = yes
interval_ms = 1000
latency_diagnostics = yes
```

Then inspect:

```bash
midi-ble-rtctl latency
midi-ble-rtctl latency-top --interval 1000
```

## 13. Troubleshooting checklist

### Device does not appear in scan

- confirm that the device is in BLE-MIDI mode;
- keep the device near the computer;
- restart Bluetooth discovery;
- check whether the device is already connected to another phone, tablet or app.

### Device appears, but probe does not find BLE-MIDI

The visible Bluetooth entry may be Audio/A2DP or another vendor-specific profile.
The generic path requires the BLE-MIDI GATT service.

### Probe finds the service, but receive is silent

- verify that `StartNotify` succeeded in daemon logs;
- confirm that the selected ALSA port is correct;
- run `aseqdump -p CLIENT:PORT` with the actual value from `aconnect -l`;
- check whether the device requires pairing before notification.

### Transmit does not work

- confirm that the characteristic has write or write-without-response support;
- test with a minimal MIDI file;
- set `enable_tx = no` if the device should only be used as an input controller.

### ALSA port does not appear

- check whether `/dev/snd/seq` is available;
- run `aconnect -l`;

## 14. When to create a device-specific profile

Keep using `standard_ble_midi` when the device follows the official BLE-MIDI UUIDs
and behaves normally.

Create a device-specific config/profile only when the device requires a quirk, for
example:

- alternative I/O characteristic UUID;
- mandatory pairing before notification;
- special connection order;
- unstable TX requiring default `enable_tx = no`;
- known recovery procedure after timeout;
- custom ALSA port naming for multiple identical devices.

The Roland GO:KEYS support is an example of a device-specific quirk profile. It
is not the conceptual target of the project; the conceptual target is any usable
BLE-MIDI device.
