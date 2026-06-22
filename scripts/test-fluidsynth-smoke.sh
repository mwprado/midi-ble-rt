#!/usr/bin/env bash
set -euo pipefail

# Hardware-free MIDI playback smoke test.
# This test sends examples/midi/test-note.mid to an existing FluidSynth ALSA
# Sequencer input port. It validates the repository MIDI example and the ALSA
# playback path without requiring a real BLE-MIDI keyboard.
#
# It does not validate BlueZ/GATT or GO:KEYS firmware behavior.

MIDI_FILE="${1:-examples/midi/test-note.mid}"
PORT="${FLUIDSYNTH_PORT:-}"

need() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "error: required command not found: $1" >&2
        exit 77
    fi
}

need aplaymidi
need aseqsend

if [[ ! -f "$MIDI_FILE" ]]; then
    echo "error: MIDI file not found: $MIDI_FILE" >&2
    exit 2
fi

if [[ -z "$PORT" ]]; then
    PORT="$(aseqsend -l | awk '/FLUID Synth|FluidSynth|fluidsynth/ {print $1; exit}')"
fi

if [[ -z "$PORT" ]]; then
    cat >&2 <<'EOF'
error: FluidSynth ALSA input port not found.

Start FluidSynth first, then re-run this test. Example:

  fluidsynth -is -m alsa_seq /usr/share/soundfonts/default.sf2

If your FluidSynth port is known, pass it explicitly:

  FLUIDSYNTH_PORT=128:0 scripts/test-fluidsynth-smoke.sh
EOF
    exit 77
fi

echo "FluidSynth playback smoke test"
echo "  input MIDI: $MIDI_FILE"
echo "  synth port: $PORT"

aplaymidi -p "$PORT" "$MIDI_FILE"

echo "PASS: aplaymidi delivered $MIDI_FILE to FluidSynth port $PORT"
