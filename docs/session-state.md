# MIDI session state model

`midi-ble-rt` does not duplicate BlueZ's Bluetooth state machine.

BlueZ remains the source of truth for adapter, device, pairing, trust, connection,
GATT service discovery and D-Bus properties. The daemon keeps only a MIDI session
state model derived from BlueZ signals plus resources owned by the daemon itself:
BLE-MIDI characteristic binding, notification subscription, parser state and ALSA
Sequencer port readiness.

## Cardinality

The daemon model is:

```text
1 daemon = 1 BlueZ adapter context + 1 ALSA Sequencer client + N MIDI sessions
1 BLE-MIDI keyboard = 1 MbSession = 1 parser state + 1 ALSA port
```

A failure, disconnect or reconnect attempt in one `MbSession` must not affect any
other active session.

## Session states

```text
IDLE
SCANNING
CONNECTING
WAIT_SERVICES
BINDING_MIDI
CREATING_ALSA
ENABLING_NOTIFY
STREAMING
RECONNECTING
ERROR
```

`STREAMING` is valid only when all of these are true for the same session:

```text
BlueZ Device1.Connected=true
BlueZ Device1.ServicesResolved=true
BLE-MIDI characteristic path is bound
Notify is enabled
ALSA port is ready
```

`Connected=true` alone is not enough. It can mean that BlueZ has a Bluetooth
connection, but it does not prove that the bridge is musically usable.

## Identity

The primary runtime key is the BlueZ `device_path`, for example:

```text
/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF
```

The stable user-facing key is the Bluetooth address:

```text
AA:BB:CC:DD:EE:FF
```

Name and alias are diagnostics only. They must not be used as decisive MIDI
identity because a device may expose confusing names such as `Audio` and `MIDI`.

## Ownership boundary

BlueZ owns:

```text
Adapter1
Device1
Connected
Paired
Trusted
ServicesResolved
GattService1
GattCharacteristic1
```

`midi-ble-rt` owns:

```text
MbSession
BLE-MIDI characteristic selection
Notify lifecycle decision
BLE-MIDI parser state
ALSA Sequencer port mapping
Reconnect policy
```

## Tests

The unit tests cover:

```text
single-session happy path
BlueZ disconnect -> RECONNECTING
independence between two sessions
error path for missing MIDI characteristic
session removal and index cleanup
invalid transition handling
```

Run them with:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```
