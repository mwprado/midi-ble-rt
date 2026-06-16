# BlueZ patch notes

## Problem

BlueZ's BLE-MIDI profile expects the official BLE-MIDI I/O characteristic:

```text
7772e5db-3868-4112-a1a9-f2669d106bf3
```

The tested Roland GO:KEYS advertises the standard BLE-MIDI service:

```text
03b80e5a-ede8-4b33-a751-6ce34ec4c700
```

but exposes the real MIDI I/O characteristic as:

```text
00006bf3-0000-1000-8000-00805f9b34fb
```

## Proposed compatibility rule

Inside the standard BLE-MIDI service, accept the Roland alias if it has the expected transport flags:

```text
notify
write-without-response
```

Preferred matching order:

1. official BLE-MIDI I/O UUID;
2. Roland `00006bf3...` alias;
3. fallback: any characteristic inside the BLE-MIDI service with `notify` and `write-without-response`, behind an experimental/quirk path.

## Minimal patch idea

```c
#define MIDI_IO_UUID              "7772E5DB-3868-4112-A1A9-F2669D106BF3"
#define MIDI_IO_ROLAND_ALIAS_UUID "00006BF3-0000-1000-8000-00805F9B34FB"
```

Then treat either as MIDI I/O only when the parent service is the standard BLE-MIDI service.
