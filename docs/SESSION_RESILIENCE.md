# Session resilience and auto reconnect

This document describes the current operational timing and state policy used by
`midi-ble-rtd` when a BLE-MIDI device is offline, powered off, slow to resolve
GATT services, or temporarily unavailable.

The goal is to keep the daemon state explicit and observable. A failed connection
attempt must not leave the session permanently stuck in `CONNECTING`; with
automatic reconnect enabled, it should move to `RECONNECTING`, keep exporting
statistics, and try again periodically.

## Configuration

Automatic reconnect is controlled by the device config:

```ini
[device]
auto_reconnect = yes
```

When `auto_reconnect = yes`, recoverable transport failures move the session to
`RECONNECTING`. The daemon remains alive and retries connection attempts by timer.

When `auto_reconnect = no`, those failures are treated as terminal for the
current daemon run and the session moves to `ERROR`.

## Timing constants

The current orchestrator uses these timings:

| Operation | Timeout / interval | Meaning |
| --- | ---: | --- |
| `Device1.Connect()` | 30 s | Maximum time allowed for BlueZ to connect the device. |
| `ServicesResolved` wait | 15 s | Maximum time to wait for GATT service discovery after connection. |
| `StartNotify` | 15 s | Maximum time allowed to enable notifications on the BLE-MIDI characteristic. |
| `WriteValue` | 5 s | Maximum time allowed for a BLE-MIDI TX packet write. |
| Device health check | 1 s | Polls `Device1.Connected` as a fallback in case the D-Bus signal is delayed or missed. |
| Reconnect timer | 10 s | Retry interval while the session is in `RECONNECTING`. |
| Stats export | Configured interval, default 1 s | Periodic stats export. Stats are also exported immediately on session transitions. |

These are operational values, not BLE-MIDI protocol constants. They may later be
moved into config if needed.

## State policy

Expected normal startup path:

```text
IDLE
-> CONNECTING
-> WAIT_SERVICES
-> BINDING_MIDI
-> CREATING_ALSA
-> ENABLING_NOTIFY
-> STREAMING
```

Expected reconnect path when the keyboard is unavailable:

```text
CONNECTING
-> RECONNECTING
-> CONNECTING
-> ...
```

Expected reconnect path after the keyboard comes back:

```text
RECONNECTING
-> CONNECTING
-> WAIT_SERVICES
-> BINDING_MIDI
-> CREATING_ALSA
-> ENABLING_NOTIFY
-> STREAMING
```

## Recoverable failures

With `auto_reconnect = yes`, these failures are recoverable:

| Failure | Session result |
| --- | --- |
| `Device1.Connect()` failure or timeout | `RECONNECTING` |
| `ServicesResolved` timeout | `RECONNECTING` |
| `StartNotify` failure or timeout | `RECONNECTING` |
| `GattCharacteristic1.WriteValue` failure | `RECONNECTING` |
| `Device1.Connected=false` | `RECONNECTING` |
| Device health check observes disconnected device | `RECONNECTING` |

The daemon keeps the ALSA side alive where possible. The GATT binding is refreshed
on the next successful connection attempt.

## Non-fatal ALSA errors

Some ALSA events are not MIDI payload and must not kill the BLE session. The
orchestrator ignores ALSA Sequencer control events before MIDI decode.

ALSA decode or I/O errors during `STREAMING` are recorded as non-fatal session
errors and drops, but the session remains in `STREAMING` unless the runtime worker
itself fails.

## Statistics behavior

The stats file is normally written to:

```text
/run/user/$UID/midi-ble-rt/stats.tsv
```

The `state` field should reflect the current session state, for example:

```text
CONNECTING
RECONNECTING
STREAMING
ERROR
```

The daemon exports a stats snapshot:

- when stats are initialized;
- periodically, using the configured stats interval;
- immediately after any session transition;
- during cleanup, before session state is cleared.

This avoids stale telemetry such as a previous `STREAMING` value remaining in the
file after a connection timeout.

## Manual test

Start with the keyboard powered off:

```bash
rm -f /run/user/$UID/midi-ble-rt/stats.tsv
midi-ble-rtd --config ~/.config/midi-ble-rt/roland-gokeys.ini
```

Expected sequence:

```text
Session: IDLE --CMD_CONNECT--> CONNECTING
Device Connect() failed: Timeout was reached
Session: CONNECTING --BLUEZ_CONNECT_FAILED--> RECONNECTING error=CONNECT_FAILED
Reconnect timer fired.
Session: RECONNECTING --RECONNECT_TIMER--> CONNECTING
```

Check stats:

```bash
cat /run/user/$UID/midi-ble-rt/stats.tsv
```

The `state` column should show `RECONNECTING` while the keyboard is unavailable.

Then power on the keyboard. On a later reconnect timer, the session should reach:

```text
Session: ENABLING_NOTIFY --NOTIFY_OK--> STREAMING
```

The `state` column should then show `STREAMING`.

## Roland GO:KEYS operational note

For GO:KEYS, connect the MIDI/BLE-GATT path first and only then connect
Audio/A2DP if needed. If Audio is connected first, the MIDI GATT path may be
missing or unstable. The daemon validates the target by BLE-MIDI GATT service and
characteristic, not by BlueZ name or alias.
