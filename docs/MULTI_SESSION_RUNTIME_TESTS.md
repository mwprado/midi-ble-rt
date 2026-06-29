# Multi-session runtime test checklist

This checklist is intended for manual validation before merging multi-session runtime changes.

## Build

```sh
cmake --build build -j
```

Expected result: all core tests and both binaries build successfully.

## Startup

```sh
./build/midi-ble-rtd --config ~/.config/midi-ble-rt
```

Expected log markers:

```text
Runtime: config-directory multi-session
ALSA Sequencer client created: midi-ble-rt
ALSA port created for <device>: midi-ble-rt:<port>
ALSA TX thread started.
StartNotify ok for <device>.
Session[<id>]: ENABLING_NOTIFY --NOTIFY_OK--> STREAMING
```

## Thread topology

```sh
ps -T -p $(pidof midi-ble-rtd) -o pid,tid,comm
```

Expected dataplane threads:

```text
midi-ble-rt-als
midi-ble-rt-rx
midi-ble-rt-tx
```

Interpretation:

- `midi-ble-rt-als`: global ALSA poll producer for TX.
- `midi-ble-rt-rx`: per-runtime RX worker, BLE to ALSA.
- `midi-ble-rt-tx`: per-runtime TX worker, ALSA to BLE.

## Control socket

```sh
./build/midi-ble-rtctl daemon-ping
./build/midi-ble-rtctl daemon-status
./build/midi-ble-rtctl daemon-list
```

Expected output shape:

```text
OK PONG
OK STATUS devices=<n> streaming=<n> lifecycle_busy=no lifecycle_queue=0
OK LIST
id	address	name	state	alsa_port
...	STREAMING	<port>
```

## RX path: BLE to ALSA

```sh
aseqdump -p midi-ble-rt:0
```

Play notes or controls on the BLE-MIDI device.

Expected result: MIDI events appear in `aseqdump`.

## TX path: ALSA to BLE

```sh
aplaymidi -p midi-ble-rt:0 <file.mid>
```

Expected result: the BLE-MIDI device receives MIDI events.

## Shutdown

Stop the daemon with Ctrl-C.

Expected log marker:

```text
ALSA TX thread stopped.
Daemon loop: stopping.
```

## Regression cases

These should be checked when changing runtime/thread code:

1. `enable_tx = no` should not create `midi-ble-rt-als` and should not break RX.
2. Repeated connection/recheck should not create more than one ALSA TX thread.
3. Disconnect/reconnect should keep one global ALSA TX thread and restore STREAMING.
4. Unknown ALSA destination ports should be ignored safely.
5. Non-MIDI ALSA Sequencer events should be ignored safely.
6. Shutdown should join the ALSA TX thread before closing ALSA resources.
