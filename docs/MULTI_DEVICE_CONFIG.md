# Multi-device configuration layout

This document describes the staged configuration model for the multi-device branches.

The public configuration option remains `--config`. The value may be either a single INI file or a configuration directory.

Single-file mode remains supported and uses the existing single-device runtime path:

```bash
midi-ble-rtd --config config/roland-gokeys.ini.example
```

Directory mode is introduced as a compatibility-safe staging point:

```bash
midi-ble-rtd --config ~/.config/midi-ble-rt
```

When the `--config` argument is a directory, the daemon automatically loads:

```text
daemon.ini
devices.d/*.ini
```

At this stage, directory mode validates the directory, loads all enabled device files, builds session skeletons, resolves each configured address to a BlueZ `Device1` object path, and attempts `Device1.Connect()` for devices with `connect_on_start = yes`. It then stays alive in a GLib main loop until Ctrl-C or SIGTERM. It does not bind GATT, does not create ALSA ports and does not start multi-device streaming yet. The orchestrator will consume this model in later cuts.

`--config-dir DIR` is kept as a temporary alias during development, but `--config DIR` is the preferred interface.

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
reconnect_on_link_loss = yes
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
connect_on_start = yes
alsa_port_name = Roland GO:KEYS BLE-MIDI

[policy]
pair = no
trust = yes
reconnect_on_link_loss = yes

[midi]
enable_tx = yes
```

Connection policy names:

```text
connect_on_start
  If yes, the daemon may initiate connection when it starts or when the device model is activated.

reconnect_on_link_loss
  If yes, the daemon may reconnect after unexpected link loss. It must not override a manual disconnect.

manual connect
  A user command. It may connect even when connect_on_start = no.

manual disconnect
  A user command. It sets desired state to disconnected and suppresses automatic reconnect.
```

Compatibility aliases still accepted by the parser:

```text
autoconnect    -> connect_on_start
auto_reconnect -> reconnect_on_link_loss
```

Rules:

```text
Only *.ini files in devices.d are loaded as device configs.
Files are loaded in lexicographic filename order.
The id field must be unique among enabled device configs.
If an id is duplicated, the first config wins and the duplicate is ignored with a warning.
Devices with enabled = no are ignored.
Devices without address are ignored.
Device identity is address-based; name is diagnostic.
The profile field is declarative for now and will drive quirk policy later.
```

Duplicate warning format:

```text
Config /path/to/devices.d/example.ini: id 'example' already configured; ignoring duplicate configuration.
```

## Current branch behavior

```bash
midi-ble-rtd --config ~/.config/midi-ble-rt
```

prints a summary like:

```text
midi-ble-rtd
Runtime: config-directory connect skeleton
Config: /home/user/.config/midi-ble-rt
ALSA client: midi-ble-rt
Service UUID: 03b80e5a-ede8-4b33-a751-6ce34ec4c700
I/O UUID: 7772e5db-3868-4112-a1a9-f2669d106bf3
Configured devices: 2
Skeleton sessions: 2

Device[0]: roland-gokeys
  enabled:        yes
  address:        CB:81:F4:62:FF:07
  profile:        roland_gokeys
  connect_on_start: yes
  bluez:          found
  session path:   /org/bluez/hci0/dev_CB_81_F4_62_FF_07
  session state:  WAIT_SERVICES

Device[1]: standard-ble-midi
  enabled:        yes
  address:        AA:BB:CC:DD:EE:FF
  profile:        standard_ble_midi
  connect_on_start: no
  bluez:          not found
  session path:   config:standard-ble-midi
  session state:  IDLE

Daemon loop: running. Press Ctrl-C to exit.
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
