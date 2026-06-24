# Structured logging and journalctl

`midi-ble-rt` uses structured GLib logging for daemon events. When the daemon runs
under systemd, these records are captured by the journal and can be inspected with
`journalctl`.

The log identifier is:

```text
midi-ble-rtd
```

Basic live view:

```bash
journalctl --user -t midi-ble-rtd -f
```

System service live view:

```bash
journalctl -t midi-ble-rtd -f
```

Unit-specific view:

```bash
journalctl --user -u midi-ble-rtd.service -f
```

## Structured fields

The structured logger emits standard journal fields plus project fields:

```text
MESSAGE
PRIORITY
SYSLOG_IDENTIFIER=midi-ble-rtd
MIDI_BLE_RT_COMPONENT
MIDI_BLE_RT_EVENT
MIDI_BLE_RT_DEVICE_ADDRESS
MIDI_BLE_RT_DEVICE_PATH
MIDI_BLE_RT_SESSION_STATE
MIDI_BLE_RT_ERROR
```

The intended component names are:

```text
daemon
config
bluez
gatt
alsa
midi
buffer
session
```

The intended event names are stable lowercase tokens, for example:

```text
startup
config_loaded
device_candidate
device_selected
device_connect
services_resolved
gatt_service_found
gatt_characteristic_found
alsa_port_ready
notify_started
rx_packet
tx_packet
buffer_overflow
session_transition
shutdown
```

## Useful queries

Show all daemon logs:

```bash
journalctl -t midi-ble-rtd
```

Follow live daemon logs:

```bash
journalctl -t midi-ble-rtd -f
```

Show warnings and errors:

```bash
journalctl -t midi-ble-rtd -p warning..alert
```

Show logs for one keyboard address, if the journal stores the structured field:

```bash
journalctl MIDI_BLE_RT_DEVICE_ADDRESS=AA:BB:CC:DD:EE:FF
```

Show a specific event:

```bash
journalctl MIDI_BLE_RT_EVENT=buffer_overflow
```

Show session transitions:

```bash
journalctl MIDI_BLE_RT_COMPONENT=session MIDI_BLE_RT_EVENT=session_transition
```

## Design rules

Logs must be useful for diagnosis without requiring debug packet dumps.

Normal operation should log high-level lifecycle events:

```text
startup
config_loaded
device_selected
connect
services_resolved
characteristic_selected
alsa_port_ready
notify_started
shutdown
```

Packet-level dumps should remain debug-only and should not be enabled by default.

Errors should always include enough context to identify the failing layer:

```text
component=bluez event=device_connect
component=gatt  event=notify_started
component=alsa  event=alsa_port_ready
component=midi  event=rx_packet
component=buffer event=buffer_overflow
```

For multi-keyboard support, session-specific logs should include at least:

```text
MIDI_BLE_RT_DEVICE_ADDRESS
MIDI_BLE_RT_DEVICE_PATH
MIDI_BLE_RT_SESSION_STATE
```
