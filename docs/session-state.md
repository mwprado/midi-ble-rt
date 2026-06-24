# MIDI session state model

`midi-ble-rt` does not duplicate BlueZ's Bluetooth state machine.

BlueZ remains the source of truth for adapter, device, pairing, trust, connection,
GATT service discovery and D-Bus properties. The daemon keeps only a MIDI session
state model derived from BlueZ signals plus resources owned by the daemon itself:
BLE-MIDI characteristic binding, notification subscription, parser state, adaptive
session buffers and ALSA Sequencer port readiness.

## Cardinality

The daemon model is:

```text
1 daemon = 1 BlueZ adapter context + 1 ALSA Sequencer client + N MIDI sessions
1 BLE-MIDI keyboard = 1 MbSession = 1 parser state + 1 ALSA port + 6 buffer rings
```

A failure, disconnect or reconnect attempt in one `MbSession` must not affect any
other active session.

## Adaptive circular buffers

Each `MbSession` owns six bounded circular buffers implemented over fixed arrays:

```text
RX path:
  rx_raw       -> raw BLE-MIDI notification bytes
  rx_decoded   -> decoded/normalized MIDI data
  rx_play      -> data ready for ALSA emission or emission audit

TX path:
  tx_raw       -> ALSA-side input data
  tx_decoded   -> decoded/normalized MIDI data
  tx_transmit  -> BLE-MIDI packets ready for GATT WriteValue
```

Each ring uses:

```text
16 slots
256 bytes physical capacity per slot
64 bytes initial logical limit
128 bytes intermediate logical limit
256 bytes maximum logical limit
```

The ring is index-based:

```text
slots[16]
read_idx
write_idx
count
```

No heap allocation is required on the hot path.

The growth policy is immediate:

```text
64 -> 128 -> 256
```

A ring grows when a packet exceeds the current logical limit or when pressure is
detected.

The shrink policy is conservative:

```text
256 -> 128 -> 64
```

A ring shrinks one step only after 30 seconds without pressure. The intent is to
react quickly to musical bursts and forget peaks slowly enough to avoid oscillation
inside a musical phrase.

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

The stable technical identity is the Bluetooth address:

```text
AA:BB:CC:DD:EE:FF
```

The BlueZ runtime object path is the active process key:

```text
/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF
```

The address is treated as authoritative. If the same address appears again with a
different BlueZ object path, the daemon must reindex the existing `MbSession`
instead of creating a duplicate session.

This matters for identical keyboards. Two Roland GO:KEYS units may expose the same
name and the same BLE-MIDI GATT shape, but they still have different Bluetooth
addresses:

```text
GO:KEYS 11:22:33:44:55:66 -> Session A -> ALSA port A
GO:KEYS AA:BB:CC:DD:EE:FF -> Session B -> ALSA port B
```

Name and alias are diagnostics only. They must not be used as decisive MIDI
identity because a device may expose confusing names such as `Audio` and `MIDI`,
or because two equal keyboards may share the same human-readable name.

An optional user index such as `gokeys-1` or `gokeys-2` is acceptable as a label
for logs, config files and ALSA port names, but it is not identity. The daemon must
not select a device by index unless that index resolves to a stored Bluetooth
address.

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
Adaptive circular MIDI buffers
ALSA Sequencer port mapping
Reconnect policy
```

## Tests

The unit tests cover:

```text
single-session happy path
BlueZ disconnect -> RECONNECTING
independence between two sessions
identical keyboard names with different addresses
duplicate address reuses/reindexes the same session
error path for missing MIDI characteristic
session removal and index cleanup
invalid transition handling
adaptive circular buffer push/pop order
ring wraparound over fixed slots
logical growth 64 -> 128 -> 256
logical shrink after 30 seconds
16-slot overflow handling
six independent rings per session
```

Run them with:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```
