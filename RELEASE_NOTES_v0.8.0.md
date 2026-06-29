midi-ble-rt v0.8.0
Highlights

This release completes the visible multi-device monitoring layer.

Add stats v5 with per-device runtime statistics.
Export one RX row and one TX row for each configured BLE-MIDI device.
Display ALSA client:port per device.
Track queue depth and peak buffer fill per device and direction.
Update midi-ble-rtctl stats and midi-ble-rtctl top to render the multi-device format.
Preserve the generic BLE-MIDI device model: instruments, controllers, modules and adapters are first-class targets.
Runtime statistics v5

The statistics file now uses a multi-row format:

v5
id address name state alsa_client_id alsa_port_id uptime_ms window_ms dir packets bytes drops ...
device-a ... RX ...
device-a ... TX ...
device-b ... RX ...
device-b ... TX ...

This replaces the previous single aggregated display for practical multi-device inspection.

Validation

Validated with the config-directory runtime and Roland GO:KEYS as a BLE-MIDI device:

daemon reaches STREAMING;
midi-ble-rtctl daemon-list reports the configured device;
midi-ble-rtctl stats renders Format: v5;
midi-ble-rtctl top renders per-device RX/TX rows;
TX traffic reports non-zero packets and peak buffer fill;
ALSA endpoint displays as 128:0 instead of 128:-1.
Fedora / COPR

The Fedora spec is updated to:

Version: 0.8.0
Release: 1%{?dist}

COPR should build from tag:

v0.8.0
