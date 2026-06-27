# Multi-device configuration layout

This document describes the staged configuration model for the `multi-device-runtime` branch.

The existing single-file mode remains supported:

```bash
midi-ble-rtd --config config/roland-gokeys.ini.example
```

The new directory mode is introduced as a compatibility-safe staging point:

```bash
midi-ble-rtd --config-dir ~/.config/midi-ble-rt
```

At this stage, `--config-dir` validates the directory and lists configured devices. It does not start multi-device streaming yet. The orchestrator will consume this model in a later cut.

## Directory shape

```text
~/.config/midi-ble-rt/
├── daemon.ini
└── devices.d/
    ├── roland-gokeys.ini
    └── standard-ble-midi.ini
```

Installed examples are expected to follow the same shape under the package data directory:

```text
/usr/share/midi-ble-rt/config/
├── daemon.ini.example
└── devices.d/
    ├── roland-gokeys.ini.example
    └── standard-ble-midi.ini.example
```

## Global daemon config

`daemon.ini` contains global defaults:

```ini
[daemon]
client_name = midi-ble-rt

[defaults]
pair = no
trust = yes
auto_reconnect = yes
enable_tx = yes
profile = standard_ble_midi

[gatt]
service_uuid = 03b80e5a-ede8-4b33-a751-6ce34ec4c700
io_uuid = 7772e5db-3868-4112-a1a9-f2669d106bf3
io_uuid_alias =
require_notify = yes
require_write_without_response = yes

[stats]
enabled = yes
interval_ms = 1000

[debug]
print_ble_packets = no
print_midi_events = no
```

Global defaults are inherited by entries in `devices.d/*.ini` unless the device file overrides them.

## Device config

Each file in `devices.d/*.ini` describes one BLE-MIDI device:

```ini
[device]
id = roland-gokeys
enabled = yes
address = CB:81:F4:62:FF:07
name = Roland GO:KEYS
profile = roland_gokeys
autoconnect = yes
alsa_port_name = Roland GO:KEYS BLE-MIDI

[policy]
pair = no
trust = yes
auto_reconnect = yes

[midi]
enable_tx = yes
```

Rules:

```text
Only *.ini files are loaded.
Devices with enabled = no are ignored.
Devices without address are ignored.
Device identity is address-based; name is diagnostic.
The profile field is declarative for now and will drive quirk policy later.
```

## Current branch behavior

```bash
midi-ble-rtd --config-dir ~/.config/midi-ble-rt
```

prints a summary like:

```text
midi-ble-rtd
Runtime: config-dir loader
Config dir: /home/user/.config/midi-ble-rt
ALSA client: midi-ble-rt
Service UUID: 03b80e5a-ede8-4b33-a751-6ce34ec4c700
I/O UUID: 7772e5db-3868-4112-a1a9-f2669d106bf3
Devices: 2

Device[0]: roland-gokeys
  enabled:        yes
  address:        CB:81:F4:62:FF:07
  profile:        roland_gokeys
  autoconnect:    yes
```

The next implementation step is to move this model into the orchestrator:

```text
one daemon process
one shared BlueZ bus
one ALSA client
one DeviceSession per enabled device
one ALSA port per enabled device
one notify subscription per streaming device
```
