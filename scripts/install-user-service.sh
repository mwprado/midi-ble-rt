#!/usr/bin/env bash
set -euo pipefail

# Install midi-ble-rtd as a systemd --user service.
#
# This installer intentionally runs in user scope. The daemon publishes an ALSA
# Sequencer port for the user's session and talks to BlueZ through D-Bus.
# Pairing/trusting/connecting can still be prepared explicitly with
# midi-ble-rtctl before the service starts.

PREFIX="${PREFIX:-$HOME/.local}"
BIN_DIR="$PREFIX/bin"
UNIT_DIR="$HOME/.config/systemd/user"
CONFIG_PATH="${MIDI_BLE_RT_CONFIG:-$HOME/.config/midi-ble-rt/roland-gokeys.ini}"
SERVICE_NAME="midi-ble-rtd.service"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DAEMON_SRC="${MIDI_BLE_RTD_BIN:-$REPO_ROOT/build/midi-ble-rtd}"
CTL_SRC="${MIDI_BLE_RTCTL_BIN:-$REPO_ROOT/build/midi-ble-rtctl}"

usage() {
    cat <<EOF
Usage: $0 [--no-enable] [--start]

Installs:
  $BIN_DIR/midi-ble-rtd
  $BIN_DIR/midi-ble-rtctl
  $UNIT_DIR/$SERVICE_NAME

Environment overrides:
  PREFIX              default: $HOME/.local
  MIDI_BLE_RT_CONFIG  default: $HOME/.config/midi-ble-rt/roland-gokeys.ini
  MIDI_BLE_RTD_BIN    default: ./build/midi-ble-rtd
  MIDI_BLE_RTCTL_BIN  default: ./build/midi-ble-rtctl

Options:
  --no-enable         install but do not enable the user service
  --start             start/restart the service after install
EOF
}

ENABLE=1
START=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-enable)
            ENABLE=0
            ;;
        --start)
            START=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

if ! command -v systemctl >/dev/null 2>&1; then
    echo "error: systemctl not found" >&2
    exit 1
fi

if [[ ! -x "$DAEMON_SRC" ]]; then
    echo "error: daemon binary not found or not executable: $DAEMON_SRC" >&2
    echo "hint: build first with: cmake --build build" >&2
    exit 1
fi

if [[ ! -x "$CTL_SRC" ]]; then
    echo "error: control binary not found or not executable: $CTL_SRC" >&2
    echo "hint: build first with: cmake --build build" >&2
    exit 1
fi

mkdir -p "$BIN_DIR" "$UNIT_DIR"
install -m 0755 "$DAEMON_SRC" "$BIN_DIR/midi-ble-rtd"
install -m 0755 "$CTL_SRC" "$BIN_DIR/midi-ble-rtctl"

cat > "$UNIT_DIR/$SERVICE_NAME" <<EOF
[Unit]
Description=midi-ble-rt BLE-MIDI to ALSA Sequencer daemon
Documentation=https://github.com/mwprado/midi-ble-rt
After=default.target

[Service]
Type=simple
ExecStart=$BIN_DIR/midi-ble-rtd --config $CONFIG_PATH
Restart=on-failure
RestartSec=2s

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload

if [[ "$ENABLE" -eq 1 ]]; then
    systemctl --user enable "$SERVICE_NAME"
fi

if [[ "$START" -eq 1 ]]; then
    systemctl --user restart "$SERVICE_NAME"
fi

cat <<EOF
Installed $SERVICE_NAME

Commands:
  systemctl --user status $SERVICE_NAME
  journalctl --user -u $SERVICE_NAME -f
  systemctl --user restart $SERVICE_NAME
  systemctl --user stop $SERVICE_NAME

Before starting the service, prepare the GO:KEYS connection/config when needed:
  midi-ble-rtctl connect CB:81:F4:62:FF:07 --profile roland_gokeys --write-config

Then:
  systemctl --user start $SERVICE_NAME
EOF
