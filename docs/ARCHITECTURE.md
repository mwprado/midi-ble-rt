# Architecture

```text
BlueZ bluetoothd
  └── Device1 / GattService1 / GattCharacteristic1
        ↓
midi-ble-rtd
  ├── config parser
  ├── device connect/trust
  ├── GATT discovery
  ├── StartNotify path
  ├── BLE-MIDI decoder
  └── ALSA Sequencer source port
        ↓
PipeWire / JACK / DAW / softsynth
```

## Future fast path

The current path uses `StartNotify()` and D-Bus `PropertiesChanged(Value)`. The future low-jitter path should prefer `AcquireNotify()` when BlueZ exposes it for the characteristic.

```text
StartNotify path:
  BLE notify -> bluetoothd -> D-Bus PropertiesChanged -> daemon -> ALSA

AcquireNotify path:
  BLE notify -> bluetoothd -> fd -> daemon RT thread -> ALSA
```

## Realtime goals

The realtime implementation should avoid aggressive scheduling for the whole process. Only the MIDI dispatch thread should use RT scheduling.

Planned RT features:

```text
mlockall(MCL_CURRENT | MCL_FUTURE)
SCHED_FIFO for MIDI dispatch thread
preallocated ring buffer
no malloc/free in hot path
no printf in hot path
latency and jitter counters
```
