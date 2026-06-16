# Roland GO:KEYS BLE-MIDI notes

## Observed GATT tree

```text
Primary Service
  03b80e5a-ede8-4b33-a751-6ce34ec4c700

Characteristic
  00000318-0000-1000-8000-00805f9b34fb
  Flags: write notify

Characteristic
  00006bf3-0000-1000-8000-00805f9b34fb
  Flags: read write-without-response write notify
  MTU: 96

Characteristic
  00009bb3-0000-1000-8000-00805f9b34fb
  Flags: write
```

The real MIDI notification stream was observed on:

```text
00006bf3-0000-1000-8000-00805f9b34fb
```

Example packets:

```text
b0 e3 90 28 01
b3 84 80 28 40
```

Decoded:

```text
90 28 01  -> Note On,  note 0x28, velocity 1
80 28 40  -> Note Off, note 0x28, velocity 64
```

## Operational order

The Roland manual/observed behavior suggests connecting MIDI before Audio. If Audio/A2DP is connected first, the keyboard may behave as a Bluetooth speaker and the MIDI path may be unavailable or unstable.
