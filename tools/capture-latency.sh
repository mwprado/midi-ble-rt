#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
Usage:
  tools/capture-latency.sh OUTDIR [SECONDS] [INTERVAL_SECONDS]

Description:
  Copy the live midi-ble-rt latency.tsv file into OUTDIR once per interval.
  The expected layout is one scenario per directory, for example:

    measurements/pre-rtkit/chords/
    measurements/rtkit-p1/chords/
    measurements/rtkit-p1/aplaymidi/

Environment:
  LATENCY_FILE  Optional source file. Defaults to:
                $XDG_RUNTIME_DIR/midi-ble-rt/latency.tsv

Examples:
  tools/capture-latency.sh measurements/pre-rtkit/chords 30
  tools/capture-latency.sh measurements/rtkit-p1/aplaymidi 30 1
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

if [[ $# -lt 1 || $# -gt 3 ]]; then
    usage
    exit 2
fi

outdir="$1"
seconds="${2:-30}"
interval="${3:-1}"
source_file="${LATENCY_FILE:-${XDG_RUNTIME_DIR:-/tmp}/midi-ble-rt/latency.tsv}"

if [[ ! "$seconds" =~ ^[0-9]+$ || "$seconds" -lt 1 ]]; then
    echo "Invalid SECONDS: $seconds" >&2
    exit 2
fi

if [[ ! "$interval" =~ ^[0-9]+$ || "$interval" -lt 1 ]]; then
    echo "Invalid INTERVAL_SECONDS: $interval" >&2
    exit 2
fi

if [[ ! -r "$source_file" ]]; then
    echo "Could not read latency diagnostics file: $source_file" >&2
    echo "Start midi-ble-rtd with [stats] latency_diagnostics = yes first." >&2
    exit 1
fi

mkdir -p "$outdir"

samples=$(( (seconds + interval - 1) / interval ))
echo "Capturing $samples samples from $source_file into $outdir" >&2

for i in $(seq 1 "$samples"); do
    ts="$(date +%Y%m%d-%H%M%S)"
    cp "$source_file" "$outdir/latency-$i-$ts.tsv"
    sleep "$interval"
done
