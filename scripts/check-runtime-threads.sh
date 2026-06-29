#!/usr/bin/env sh
set -eu

DAEMON=${DAEMON:-midi-ble-rtd}
CTL=${CTL:-./build/midi-ble-rtctl}

pid=$(pidof "$DAEMON" 2>/dev/null || true)
if [ -z "$pid" ]; then
    echo "ERR $DAEMON is not running" >&2
    exit 1
fi

echo "Threads for $DAEMON pid=$pid:"
ps -T -p "$pid" -o pid,tid,comm

echo
missing=0

for name in midi-ble-rt-als midi-ble-rt-rx midi-ble-rt-tx; do
    if ps -T -p "$pid" -o comm= | grep -qx "$name"; then
        echo "OK thread $name"
    else
        echo "ERR missing thread $name" >&2
        missing=1
    fi
done

echo

if [ -x "$CTL" ]; then
    status=$("$CTL" daemon-status || true)
    echo "$status"

    echo "$status" | grep -q "alsa_tx_thread=running" || {
        echo "ERR daemon-status does not report alsa_tx_thread=running" >&2
        missing=1
    }

    echo "$status" | grep -q "rx_workers=" || {
        echo "ERR daemon-status does not report rx_workers" >&2
        missing=1
    }

    echo "$status" | grep -q "tx_workers=" || {
        echo "ERR daemon-status does not report tx_workers" >&2
        missing=1
    }
else
    echo "WARN ctl not executable: $CTL" >&2
fi

exit "$missing"
