#!/usr/bin/env bash
set -euo pipefail

# Hardware-free ALSA Sequencer smoke test.
# It records from an ALSA MIDI port with aseqdump while sending the repository
# test MIDI file into the same port. The default target is Midi Through 14:0,
# which is available on normal ALSA Sequencer setups.

MIDI_FILE="${1:-examples/midi/test-note.mid}"
PORT="${MIDI_BLE_RT_TEST_PORT:-14:0}"
TIMEOUT_CMD="${TIMEOUT_CMD:-timeout}"

need() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "error: required command not found: $1" >&2
        exit 77
    fi
}

need aseqdump
need aplaymidi
need "$TIMEOUT_CMD"

if [[ ! -f "$MIDI_FILE" ]]; then
    echo "error: MIDI file not found: $MIDI_FILE" >&2
    exit 2
fi

if ! aconnect -l >/dev/null 2>&1; then
    echo "error: ALSA Sequencer is not available" >&2
    exit 77
fi

if ! aconnect -l | grep -q "Midi Through"; then
    echo "error: Midi Through port not found" >&2
    echo "hint: load snd-seq-dummy or set MIDI_BLE_RT_TEST_PORT=CLIENT:PORT" >&2
    exit 77
fi

OUT="$(mktemp -t midi-ble-rt-aseqdump.XXXXXX)"
trap 'rm -f "$OUT"' EXIT

echo "ALSA loopback smoke test"
echo "  input MIDI: $MIDI_FILE"
echo "  ALSA port:  $PORT"

"$TIMEOUT_CMD" 5s aseqdump -p "$PORT" >"$OUT" 2>&1 &
DUMP_PID=$!

sleep 0.3
aplaymidi -p "$PORT" "$MIDI_FILE"
sleep 0.7

wait "$DUMP_PID" || true

if grep -qi "Note on" "$OUT" && grep -qi "Note off" "$OUT"; then
    echo "PASS: captured Note on/off through ALSA"
    exit 0
fi

echo "FAIL: did not capture expected Note on/off" >&2
echo "--- aseqdump output ---" >&2
cat "$OUT" >&2
echo "-----------------------" >&2
exit 1
