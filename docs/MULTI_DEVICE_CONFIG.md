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
3. build one MbSession per configured device
4. resolve configured addresses to BlueZ Device1 paths
5. for devices with connect_on_start = yes:
   - pair/trust according to device policy
   - call Device1.Connect() when needed
   - wait for ServicesResolved
   - bind the BLE-MIDI service and I/O characteristic
   - create one ALSA Sequencer port for that session
   - start one RX worker and one TX worker for the session
   - enable StartNotify
6. keep one shared BlueZ system bus and one shared ALSA client
7. stay alive in a GLib main loop until Ctrl-C or SIGTERM
```

Directory mode is now a multi-session runtime foundation. The model is centered on `MbDaemon`, `MbSession` and `MbDuplexRuntime`; BlueZ and GATT are transport infrastructure only.

ALSA ports are created only after the device has a valid BlueZ path, GATT service, BLE-MIDI I/O characteristic and runtime workers. No fake ALSA port is exposed for an unbound device.

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

## Runtime shape

```text
one daemon process
one shared BlueZ system bus
one shared ALSA Sequencer client
one MbDaemon session index
one MbSession per enabled device
one MbDuplexRuntime per streaming-capable device
one ALSA port per bound device
one GATT notify subscription per bound device
```

Expected ALSA shape once multiple devices are bound:

```text
client 128: 'midi-ble-rt' [type=user]
    0 'Roland GO KEYS BLE-MIDI'
    1 'Standard BLE-MIDI Device'
    2 'CME WIDI Master BLE-MIDI'
```

Routing model:

```text
TX:
  ALSA event dest.port = 0 -> MbSession A -> GATT WriteValue char A
  ALSA event dest.port = 1 -> MbSession B -> GATT WriteValue char B

RX:
  GATT notify char A -> MbSession A RX runtime -> ALSA port 0
  GATT notify char B -> MbSession B RX runtime -> ALSA port 1
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
The profile field selects profile/quirk policy such as the Roland GO:KEYS I/O UUID alias.
```

Validation target for this development branch:

```text
--config DIR with one configured GO:KEYS device reaches STREAMING
one real ALSA port appears for that session
aplaymidi can transmit to the device through that port
GATT notify events are routed back to the same ALSA port
single-file orchestrator mode still works unchanged
```
