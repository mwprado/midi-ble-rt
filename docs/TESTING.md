# Testing

This document separates hardware-free MIDI tests from real BLE-MIDI device tests.

## Test categories

```text
ALSA loopback test
  validates: MIDI file -> ALSA Sequencer -> aseqdump capture
  requires: ALSA Sequencer, Midi Through
  does not require: FluidSynth, BlueZ, GO:KEYS

FluidSynth smoke test
  validates: MIDI file -> ALSA Sequencer -> FluidSynth input
  requires: FluidSynth running with an ALSA Sequencer input port
  does not require: BlueZ, GO:KEYS

GO:KEYS BLE-MIDI test
  validates: ALSA <-> midi-ble-rtd <-> BlueZ GATT <-> GO:KEYS
  requires: real Roland GO:KEYS
```

## ALSA loopback smoke test

Run:

```bash
scripts/test-alsa-loopback.sh
```

Default behavior:

```text
examples/midi/test-note.mid -> aplaymidi -> Midi Through 14:0 -> aseqdump
```

Expected result:

```text
PASS: captured Note on/off through ALSA
```

To use another ALSA Sequencer port:

```bash
MIDI_BLE_RT_TEST_PORT=CLIENT:PORT scripts/test-alsa-loopback.sh
```

Example:

```bash
MIDI_BLE_RT_TEST_PORT=14:0 scripts/test-alsa-loopback.sh
```

## FluidSynth playback smoke test

Start FluidSynth first. The exact SoundFont path depends on the distribution.

Common examples:

```bash
fluidsynth -is -m alsa_seq /usr/share/soundfonts/default.sf2
```

or:

```bash
fluidsynth -is -m alsa_seq /usr/share/soundfonts/FluidR3_GM.sf2
```

Then run:

```bash
scripts/test-fluidsynth-smoke.sh
```

The script tries to find a FluidSynth ALSA input port automatically. If it cannot, pass it explicitly:

```bash
FLUIDSYNTH_PORT=128:0 scripts/test-fluidsynth-smoke.sh
```

Expected result:

```text
PASS: aplaymidi delivered examples/midi/test-note.mid to FluidSynth port CLIENT:PORT
```

This test confirms that the repository MIDI example is playable and that ALSA can deliver it to a software synth. It does not validate BLE-MIDI GATT writes.

## Real GO:KEYS receive test

With the daemon running:

```bash
./build/midi-ble-rtd --config ~/.config/midi-ble-rt/roland-gokeys.ini
```

List the port:

```bash
aseqdump -l
```

Then capture:

```bash
aseqdump -p CLIENT:PORT
```

or record:

```bash
arecordmidi -p CLIENT:PORT gokeys-input.mid
```

## Real GO:KEYS transmit test

With the daemon running and the duplex port visible in `aseqsend -l`:

```bash
aplaymidi -p CLIENT:PORT examples/midi/test-note.mid
```

Expected daemon log with debug enabled:

```text
ALSA->BLE MIDI: 90 3c 64
BLE write: ...
ALSA->BLE MIDI: 80 3c 40
BLE write: ...
```

Expected result: the GO:KEYS plays the test note.

## Notes

The hardware-free tests intentionally do not mock BlueZ GATT yet. A later test layer can add a fake GATT characteristic object on D-Bus to exercise BLE packet encoding and `WriteValue` behavior without a real keyboard.
