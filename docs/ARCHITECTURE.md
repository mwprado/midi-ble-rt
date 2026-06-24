# midi-ble-rt architecture

`midi-ble-rt` exposes a single public daemon:

```text
midi-ble-rtd
```

The previous `midi-ble-rtd-duplex` path was a validation wrapper for the threaded
RX/TX runtime.  Its logic is now represented as the daemon's internal
orchestrator path.  There should not be two daemon concepts in the installed
interface.

## Layer model

```text
midi-ble-rtd
  -> mb-daemon
      -> mb-orchestrator
          -> core modules / session model / runtime queues
```

## `midi-ble-rtd`

`midi-ble-rtd` is the public executable.  Users, service files and packages
should refer to this binary.

It remains the stable command-line entry point:

```bash
midi-ble-rtd --config ~/.config/midi-ble-rt/roland-gokeys.ini
```

## `mb-daemon`

`mb-daemon` owns process-level concerns:

```text
main()
argument handling
process startup
exit code
signal/cleanup boundary
```

It should stay small.  It should not contain BLE-MIDI policy, ALSA decode logic,
BlueZ/GATT discovery logic or runtime queue mechanics.

## `mb-orchestrator`

`mb-orchestrator` owns runtime policy:

```text
session lifecycle
RX/TX coordination
threaded runtime startup/shutdown
queue push/consume decisions
ALSA polling policy
GATT notification callback policy
reconnect policy, later
multi-session policy, later
```

The orchestrator is the right place to answer questions such as:

```text
Which session is active?
When should a session transition state?
When should RX data be pushed into the runtime?
When should ALSA TX data be written to GATT?
When should reconnect be attempted?
```

It should not become the implementation owner of low-level primitives.

## Core modules

The core modules define domain objects and primitive operations:

```text
mb-session          session state and invariants
mb-buffer           adaptive session buffers
mb-runtime          runtime flow primitives
mb-duplex-runtime   threaded RX/TX runtime
mb-slice-ring       fixed-size queue primitive
mb-frame-model      frame/model helpers
mb-log              structured logging
```

The current transition still reuses some legacy static helpers from the original
daemon implementation.  The intended direction is to extract them into explicit
core modules:

```text
mb-config           config parsing
mb-bluez            Device1 discovery, pair/trust/connect
mb-gatt             BLE-MIDI service/characteristic, StartNotify, WriteValue
mb-alsa             ALSA Sequencer client/ports, encode/decode, control events
mb-ble-midi         BLE-MIDI packet encode/decode
```

## Session ownership rule

The session object belongs to the core.  Session lifecycle policy belongs to the
orchestrator.

```text
core:
  what is a session?
  what states and invariants are valid?

orchestrator:
  what should happen to this session now?
  when should it connect, notify, stream, reconnect or stop?
```

This separation makes debugging direct:

```text
argument/config failure          -> mb-daemon
state transition/reconnect issue -> mb-orchestrator / mb-session
ALSA event/decode issue          -> mb-alsa
BlueZ/GATT issue                 -> mb-bluez / mb-gatt
BLE-MIDI packet issue            -> mb-ble-midi
queue/drop/overflow issue        -> mb-buffer / mb-runtime
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
```

The daemon may cache BlueZ values for decisions.  BlueZ remains the authority for
Bluetooth truth.

## Runtime flow

```text
BLE-MIDI notification
  -> BlueZ PropertiesChanged(Value)
  -> mb-orchestrator RX callback
  -> mb-duplex-runtime RX queue
  -> BLE-MIDI decode
  -> ALSA Sequencer output

ALSA Sequencer input
  -> mb-orchestrator TX poll
  -> ALSA event filter
  -> MIDI bytes
  -> mb-duplex-runtime TX queue
  -> BLE-MIDI packet encode
  -> GATT WriteValue
```

ALSA Sequencer control events such as `PORT_SUBSCRIBED` and `PORT_UNSUBSCRIBED`
are not MIDI payload.  They must be ignored before `snd_midi_event_decode()`.

## Installed interface

The installed binaries should be:

```text
midi-ble-rtd
midi-ble-rtctl
```

`midi-ble-rtd-duplex` was useful during validation but should not be treated as a
separate installed daemon model.

## Future fast path

The current path uses `StartNotify()` and D-Bus `PropertiesChanged(Value)`. A
future lower-jitter path may prefer `AcquireNotify()` when BlueZ exposes it for
the characteristic.
