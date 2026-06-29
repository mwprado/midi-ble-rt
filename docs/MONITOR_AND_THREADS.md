# Lifecycle Monitor and RX/TX Threads

## Overview

When `midi-ble-rtd` runs in `config-directory multi-session` mode, the runtime is split into two explicit layers:

- **control-plane**: discovery, connection, reconnect, GATT/BlueZ binding, health checks and local control;
- **dataplane**: real-time MIDI flow between BLE-MIDI devices and ALSA Sequencer.

This separation keeps session management out of the MIDI hot path. The monitor owns state transitions and structural operations. RX/TX workers own MIDI data movement after a device reaches `STREAMING`.

The architecture is generic for any compatible BLE-MIDI device. Device-specific profiles, such as the Roland GO:KEYS profile, are treated as validated targets or quirks, not as the conceptual model of the runtime.

## Runtime components

### Lifecycle monitor

The lifecycle monitor is the serialized control-plane executor. It runs on the GLib main loop thread and processes commands from an internal lifecycle queue.

Typical commands are:

- `DISCOVER`
- `CONNECT`
- `DISCONNECT`
- `RECHECK`
- `MARK_DISCONNECTED`
- `SESSION_EVENT`

The monitor is responsible for:

- resolving configured devices to BlueZ `Device1` paths;
- connecting and reconnecting BLE devices;
- waiting for `ServicesResolved`;
- finding the BLE-MIDI service and I/O characteristic;
- creating and binding ALSA Sequencer ports;
- enabling `StartNotify`;
- reacting to disconnects and runtime failures;
- serving lifecycle-changing requests from the local control socket.

The monitor should not perform MIDI dataplane work. It should only move the runtime toward, away from, or around `STREAMING`.

## Direction naming

The project uses these direction names consistently:

```text
RX = BLE-MIDI -> daemon -> ALSA
TX = ALSA -> daemon -> BLE-MIDI
```

This naming is daemon-centric:

- RX means the daemon receives MIDI from BLE and emits it to ALSA;
- TX means the daemon receives MIDI from ALSA and transmits it to BLE.

## RX dataplane

RX is the BLE-MIDI-to-ALSA path.

```text
BLE-MIDI device
    -> BlueZ GATT notification
    -> RX ring
    -> midi-ble-rt-rx worker
    -> BLE-MIDI decode
    -> ALSA Sequencer output
```

The BlueZ callback only receives the GATT `Value` bytes and pushes them into the RX runtime queue. The callback must remain lightweight and must not do heavy MIDI decoding in the GLib main loop.

The `midi-ble-rt-rx` worker consumes RX queue entries, decodes BLE-MIDI packets and writes resulting MIDI events to ALSA.

## TX dataplane

TX is the ALSA-to-BLE-MIDI path.

It has two stages:

1. a global ALSA polling producer thread;
2. a per-runtime TX worker.

```text
ALSA Sequencer event
    -> midi-ble-rt-als thread
    -> route by ALSA dest.port
    -> TX ring
    -> midi-ble-rt-tx worker
    -> BLE-MIDI packet encode
    -> GATT WriteValue
    -> BLE-MIDI device
```

### Global ALSA TX producer

The global ALSA producer thread polls the shared ALSA Sequencer client and reads incoming events.

Expected thread name:

```text
midi-ble-rt-als
```

Its responsibilities are:

- wait for ALSA events using `poll()`;
- read events with `snd_seq_event_input()`;
- identify the target device by `ev->dest.port`;
- decode ALSA Sequencer events into raw MIDI bytes;
- push those MIDI bytes into the target device TX queue.

The thread is global because there is one shared ALSA Sequencer client in the multi-session runtime.

### TX worker

The per-runtime TX worker consumes raw MIDI bytes from the TX queue.

Expected thread name:

```text
midi-ble-rt-tx
```

Its responsibilities are:

- consume the device TX ring;
- packetize raw MIDI bytes into BLE-MIDI packets;
- send the packet through BlueZ GATT `WriteValue`;
- serialize writes per device using the device GATT TX lock.

The TX worker is separated from the ALSA polling thread so that ALSA event ingestion remains independent from BLE/GATT write latency.

## Simple diagram

```text
                    +----------------------------------+
                    |      GLib Main Loop / Monitor    |
                    |----------------------------------|
                    | discover / connect / recheck     |
                    | bind BLE-MIDI / ALSA             |
                    | StartNotify / reconnect / ctl    |
                    +----------------+-----------------+
                                     |
                                     |
                         session reaches STREAMING
                                     |
         -------------------------------------------------------------
         |                                                           |
         v                                                           v

+--------------------------+                         +-----------------------------+
|        RX path           |                         |           TX path           |
|     BLE-MIDI -> ALSA     |                         |        ALSA -> BLE-MIDI     |
+--------------------------+                         +-----------------------------+

BlueZ GATT notification                              ALSA Sequencer event
          |                                                     |
          v                                                     v
   RX ring / queue                                    thread midi-ble-rt-als
          |                                           (poll/read ALSA events)
          v                                                     |
 thread midi-ble-rt-rx                                          v
 (BLE-MIDI decode)                                       TX ring / queue
          |                                                     |
          v                                                     v
    ALSA Sequencer                                       thread midi-ble-rt-tx
                                                         (BLE-MIDI encode)
                                                                  |
                                                                  v
                                                           GATT WriteValue
```

## Per-direction diagrams

### RX

```text
Compatible BLE-MIDI device
        |
        v
BlueZ notify callback
        |
        v
mb_duplex_runtime_push_rx(...)
        |
        v
RX queue
        |
        v
midi-ble-rt-rx
        |
        v
BLE-MIDI decode
        |
        v
ALSA Sequencer
```

### TX

```text
ALSA Sequencer
        |
        v
midi-ble-rt-als
(poll + snd_seq_event_input)
        |
        v
route by dest.port
        |
        v
mb_duplex_runtime_push_tx(...)
        |
        v
TX queue
        |
        v
midi-ble-rt-tx
        |
        v
BLE-MIDI encode
        |
        v
GATT WriteValue
        |
        v
Compatible BLE-MIDI device
```

## Expected runtime threads

With one device in `STREAMING`, the process should normally show:

```text
midi-ble-rt-als
midi-ble-rt-rx
midi-ble-rt-tx
```

Interpretation:

- `midi-ble-rt-als`: global ALSA polling producer for TX;
- `midi-ble-rt-rx`: RX worker for BLE-MIDI to ALSA;
- `midi-ble-rt-tx`: TX worker for ALSA to BLE-MIDI.

The control-plane still runs on the GLib main loop thread.

## Runtime properties

### Serialized control-plane

Lifecycle commands are serialized through the monitor. This prevents discovery, connection, reconnect and disconnect operations from racing each other.

### Parallel dataplane

After `STREAMING`, RX and TX flows run independently from lifecycle processing. MIDI traffic should not be handled by the lifecycle queue.

### Per-device TX locking

GATT `WriteValue` calls are serialized per device. This avoids concurrent writes to the same BLE-MIDI characteristic while still allowing different devices to progress independently.

### ALSA routing by destination port

The global ALSA TX thread uses `ev->dest.port` to select the target runtime. Unknown ports are ignored safely.

### Clean shutdown ordering

Shutdown should stop runtime activity before closing shared resources:

```text
1. stop GLib sources/timers
2. stop ALSA TX producer thread
3. stop per-device RX/TX workers
4. free per-device ALSA MIDI encoders/decoders
5. close ALSA Sequencer client
6. unref BlueZ/system bus resources
```

## Validation

Useful commands:

```sh
./build/midi-ble-rtctl daemon-status
./build/midi-ble-rtctl daemon-list
./scripts/check-runtime-threads.sh
```

Expected status shape for one streaming device:

```text
OK STATUS devices=1 streaming=1 alsa_tx_thread=running rx_workers=1 tx_workers=1 lifecycle_busy=no lifecycle_queue=0
```

Expected thread check markers:

```text
OK thread midi-ble-rt-als
OK thread midi-ble-rt-rx
OK thread midi-ble-rt-tx
```

## Future maintenance rules

Do not regress the separation between monitor and dataplane:

- do not move BLE-MIDI decode back into the BlueZ callback;
- do not move ALSA polling back into a high-frequency GLib timer;
- do not perform GATT `WriteValue` from the ALSA polling thread;
- keep RX/TX naming consistent;
- keep Roland-specific behavior behind profile/quirk logic, not in the generic runtime model.

In one sentence:

```text
The monitor serializes control-plane state; RX/TX threads carry the MIDI dataplane.
```
