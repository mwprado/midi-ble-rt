# Multi-device configuration layout

The public configuration option remains `--config`. The value may be either a single INI file or a configuration directory.

Single-file mode remains supported:

```bash
midi-ble-rtd --config config/roland-gokeys.ini.example
```

Directory mode loads:

```text
daemon.ini
devices.d/*.ini
```

Current directory-mode behavior:

```text
1. validate the config directory
2. load enabled device configs
3. build session skeletons
4. resolve configured addresses to BlueZ Device1 paths
5. attempt Device1.Connect() for devices with connect_on_start = yes
6. stay alive in a GLib main loop until Ctrl-C or SIGTERM
```

It does not bind GATT, does not create ALSA Sequencer ports, and does not start multi-device streaming yet. ALSA ports are exposed only by the single-device orchestrator path, where the dataplane is active.

`--config-dir DIR` is a temporary development alias. Prefer `--config DIR`.

## Directory shape

```text
~/.config/midi-ble-rt/
├── daemon.ini
└── devices.d/
    ├── roland-gokeys.ini
    └── standard-ble-midi.ini
```

Installed examples follow the same shape under:

```text
/usr/share/midi-ble-rt/config/
```

## Global daemon config

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

## Device config

Configs and packaged examples use the current key names only:

```ini
[device]
id = roland-gokeys
enabled = yes
address = CB:81:F4:62:FF:07
name = Roland GO KEYS
profile = roland_gokeys
connect_on_start = yes
alsa_port_name = Roland GO KEYS BLE-MIDI

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

The next implementation step is to move this model into the orchestrator:

```text
one daemon process
one shared BlueZ bus
one ALSA client
one DeviceSession per enabled device
one ALSA port per streaming-capable device
one notify subscription per streaming device
```
