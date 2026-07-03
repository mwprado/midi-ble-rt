# midi-ble-rt architecture

`midi-ble-rt` exposes a single public daemon:

```text
midi-ble-rtd
```

The previous duplex validation path was folded into the daemon runtime. There
should not be two daemon concepts in the installed interface.

## Layer model

```text
midi-ble-rtd
  -> process entry / argument handling
  -> config-directory runtime
      -> MbDaemon session index
      -> MbSession per device
      -> MbDuplexRuntime per streaming-capable device
      -> core modules / runtime queues
```

## `midi-ble-rtd`

`midi-ble-rtd` is the public executable. Users, service files and packages should
refer to this binary.

It remains the stable command-line entry point:

```text
midi-ble-rtd --config ~/.config/midi-ble-rt
```

The `--config` value is the configuration directory containing `daemon.ini` and
`devices.d/*.ini`.

## Process boundary

The process entry owns process-level concerns:

```text
argument handling
version reporting
process startup
exit code
signal/cleanup boundary
```

It should stay small. It should not contain BLE-MIDI policy, ALSA decode logic,
BlueZ/GATT discovery logic or runtime queue mechanics.

## Runtime policy

The daemon runtime owns policy and coordination:

```text
session lifecycle
RX/TX coordination
threaded runtime startup/shutdown
queue push/consume decisions
ALSA polling policy
GATT notification callback policy
reconnect policy
multi-session policy
control socket handling
stats and latency snapshot export
```

The runtime is the right place to answer questions such as:

```text
Which session is active?
When should a session transition state?
When should RX data be pushed into the runtime?
When should ALSA TX data be written to GATT?
When should reconnect be attempted?
Which ALSA port maps to which configured device?
```

It should not become the implementation owner of low-level primitives.

## Core modules

The core modules define domain objects and primitive operations:

```text
mb-session              session state and invariants
mb-alsa                 ALSA Sequencer event classification
mb-alsa-port            ALSA port helpers
mb-bluez                BlueZ Device1 operations
mb-gatt-midi            BLE-MIDI service/characteristic operations
mb-config               runtime config parsing and defaults
mb-ble-midi             BLE-MIDI UUIDs and packet helpers
mb-buffer               adaptive session buffers
mb-runtime              runtime flow primitives
mb-duplex-runtime       threaded RX/TX runtime
mb-slice-ring           fixed-size queue primitive
mb-frame-model          frame/model helpers
mb-log                  structured logging
mb-stats                stats snapshot export
mb-latency-diagnostics  latency snapshot export
mb-rtkit                optional RealtimeKit scheduling helper
```

New functionality should be added to an explicit core module when it is a
primitive, or to the daemon runtime when it is process/session policy.

## Session ownership rule

The session object belongs to the core. Session lifecycle policy belongs to the
daemon runtime.

```text
core:
  what is a session?
  what states and invariants are valid?

runtime:
  what should happen to this session now?
  when should it connect, notify, stream, reconnect or stop?
```

This separation makes debugging direct:

```text
argument/config failure          -> process entry / mb-config
state transition/reconnect issue -> daemon runtime / mb-session
ALSA event/decode issue          -> mb-alsa
BlueZ/GATT issue                 -> mb-bluez / mb-gatt-midi
BLE-MIDI packet issue            -> mb-ble-midi
queue/drop/overflow issue        -> mb-buffer / mb-runtime
RtKit scheduling issue           -> mb-rtkit
```

## BlueZ boundary

The daemon does not duplicate BlueZ's Bluetooth state machine.

BlueZ owns:

```text
Adapter1
Device1
Paired
Trusted
Connected
ServicesResolved
GattService1
GattCharacteristic1
```

`midi-ble-rt` owns:

```text
MbSession
BLE-MIDI characteristic binding
Notify lifecycle decision
BLE-MIDI parser state
ALSA Sequencer port mapping
runtime queues
reconnect policy
stats and latency export
optional realtime worker scheduling
```

The daemon may cache BlueZ values for decisions. BlueZ remains the authority for
Bluetooth truth.

## Runtime flow

```text
BLE-MIDI notification
  -> BlueZ PropertiesChanged(Value)
  -> daemon RX callback
  -> mb-duplex-runtime RX queue
  -> BLE-MIDI decode
  -> ALSA Sequencer output

ALSA Sequencer input
  -> daemon ALSA TX poll
  -> ALSA event filter
  -> MIDI bytes
  -> mb-duplex-runtime TX queue
  -> mb-ble-midi packet encode
  -> GATT WriteValue
```

ALSA Sequencer control events such as `PORT_SUBSCRIBED` and `PORT_UNSUBSCRIBED`
are not MIDI payload. They must be ignored before `snd_midi_event_decode()`.

## Installed interface

The installed binaries are:

```text
midi-ble-rtd
midi-ble-rtctl
```

`midi-ble-rtd-duplex` was useful during validation but is not an installed daemon
model.

## Future fast path

The current path uses `StartNotify()` and D-Bus `PropertiesChanged(Value)`. A
future lower-jitter path may prefer `AcquireNotify()` when BlueZ exposes it for
the characteristic.
