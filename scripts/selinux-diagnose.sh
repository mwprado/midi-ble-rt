#!/usr/bin/env bash
set -euo pipefail

echo "== SELinux status =="
getenforce || true

echo
echo "== Relevant process contexts =="
ps -eZ | grep -E 'bluetoothd|midi-ble-rtd' || true

echo
echo "== ALSA sequencer label =="
ls -Z /dev/snd/seq || true

echo
echo "== Recent bluetoothd AVCs =="
sudo ausearch -c bluetoothd -m avc,user_avc -ts recent || true

echo
echo "== Recent midi-ble-rtd AVCs =="
sudo ausearch -c midi-ble-rtd -m avc,user_avc -ts recent || true

echo
echo "== Hint =="
echo "If BlueZ native MIDI works only with SELinux disabled, test:"
echo "  sudo semanage permissive -a bluetooth_t"
echo "  # retry"
echo "  sudo semanage permissive -d bluetooth_t"
