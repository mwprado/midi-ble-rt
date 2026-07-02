# BLE-MIDI fragmentation and resync policy

This document records the intended RX/TX safety policy for BLE-MIDI packet fragmentation in `midi-ble-rt`.

> Status: the implementation work is currently on the `dataplane-fence` branch. At the time this document was added, `dataplane-fence` had not yet been merged into `master`.

## Core rule

A BLE notification is a transport frame, not necessarily a complete MIDI message.

One BLE-MIDI packet may contain:

- a complete MIDI message;
- multiple MIDI messages;
- only the first part of a MIDI message;
- the continuation of a MIDI message started in a previous packet;
- the end of one message and the start of another;
- a realtime MIDI byte interleaved inside another message;
- part of a long SysEx transfer.

Therefore the RX decoder must be incremental and stateful per configured BLE-MIDI device.

## Decoder state

The decoder must preserve enough state across BLE packets to reconstruct valid MIDI streams:

- `running_status` for channel voice messages;
- pending short message status;
- pending data bytes for incomplete short messages;
- expected pending data length;
- SysEx active state between `F0` and `F7`.

The decoder must not treat the end of a BLE packet as the end of a MIDI message.

## Fragmented short messages

A short channel/system message is emitted only after all required data bytes have arrived.

Example:

```text
packet 1: 90 3C
packet 2: 7F
```

This must emit exactly:

```text
90 3C 7F
```

The partial `90 3C` must not be emitted early and must not be discarded merely because the BLE packet ended.

## Running status

Running status may span BLE packet boundaries.

Example:

```text
packet 1: 90 3C 7F 40
packet 2: 7F
```

This represents:

```text
90 3C 7F
90 40 7F
```

A valid `running_status` may be used to complete later data bytes, but only while the stream remains trustworthy.

## SysEx

SysEx starts at `F0` and ends at `F7`.

Example:

```text
packet 1: F0 41 10
packet 2: 42 12 F7
```

The decoder must preserve `sysex_active` across BLE packets and emit the complete SysEx stream:

```text
F0 41 10 42 12 F7
```

Bytes below `0x80` inside an active SysEx are data bytes and must not be discarded merely because there is no `running_status`.

## Realtime bytes

MIDI realtime bytes may appear between bytes of another MIDI message and must not break the pending message state.

Example:

```text
packet 1: 90 3C
packet 2: F8 7F
```

This should emit realtime immediately while preserving the pending Note On:

```text
F8
90 3C 7F
```

## Fail-closed resync policy

When stream integrity becomes ambiguous, the daemon must fail closed:

1. reset the BLE-MIDI decoder state;
2. drop the suspect fragment;
3. do not complete an old pending MIDI message with new bytes of uncertain origin;
4. count the event as an RX/TX drop where appropriate;
5. resume only from a clear valid MIDI/BLE-MIDI boundary.

Ambiguous conditions include:

- invalid BLE-MIDI packet header;
- orphan MIDI data byte without pending message, running status or active SysEx;
- unexpected non-realtime status while a short message is pending;
- unexpected non-realtime status inside SysEx before `F7`;
- stale dataplane item rejected by the streaming epoch fence;
- stream/session transition out of `STREAMING`.

The principle is:

```text
Never complete a pending MIDI message using bytes from an untrusted frame or epoch.
```

It is better to lose one musical event than to synthesize a false event or leave a note, sustain pedal or sound generator stuck.

## MIDI panic on RX resync

When RX framing becomes unsafe, the daemon should emit a MIDI panic toward ALSA to protect downstream synthesizers and applications.

For each of the 16 MIDI channels, emit:

```text
CC 64  0   Sustain Off
CC 120 0   All Sound Off
CC 123 0   All Notes Off
Pitch Bend center
```

Byte form for one channel:

```text
B0 40 00
B0 78 00
B0 7B 00
E0 00 40
```

Repeat with channel bits from `0x0` through `0xF`.

This panic is intended for RX corruption, where the Linux ALSA side may otherwise keep a note, sustain pedal or sound state active.

## TX behavior

TX has a different failure mode. If ALSA-to-BLE transmission fails while the device is still streaming, the daemon may attempt a BLE-side panic if the link is usable. If the link has dropped, the daemon cannot deliver anything to the physical instrument; it must instead reset local state and avoid replaying stale buffered data after reconnect.

Pending TX data is a jitter buffer for the current streaming epoch, not a replay queue across BLE session loss.

## Dataplane fence

A streaming epoch fence prevents data from an old `STREAMING` session from being processed after a disconnect, reconnect or session transition.

On transition out of `STREAMING`, the daemon should:

- close the current dataplane epoch;
- drop pending RX/TX queue items for the device;
- reset partial BLE-MIDI decoder state;
- reset ALSA MIDI encode/decode state where appropriate.

A later reconnect starts a new epoch. Old queued data must not be replayed into the new session.

## Tests

The unit tests should cover at least:

- complete Note On in one packet;
- fragmented Note On across packets;
- running status across packets;
- fragmented SysEx across packets;
- realtime byte interleaved inside a pending short message;
- orphan data byte causing resync;
- invalid BLE-MIDI header causing resync;
- decoder reset discarding partial state.
