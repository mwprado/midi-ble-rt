#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

echo "==> Limpando build/"
rm -rf build

echo "==> Configurando CMake com GUI"
cmake -S . -B build -DMIDI_BLE_RT_BUILD_GUI=ON

echo "==> Compilando"
cmake --build build

echo
echo "OK: build concluído"
echo "GUI: ./build/gui/midi-ble-rt-gui"
echo "CTL: ./build/midi-ble-rtctl"
echo "Daemon: ./build/midi-ble-rtd"
