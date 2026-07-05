# midi-ble-rt

`midi-ble-rt` exposes Bluetooth LE MIDI devices as ALSA Sequencer MIDI ports on Linux.

The runtime uses BlueZ for BLE/GATT transport and ALSA Sequencer as the public MIDI interface. PipeWire and DAWs can consume the exported ALSA MIDI ports, but PipeWire is not part of the core bridge.

The first validated hardware target is the Roland GO:KEYS family. The project target remains generic: any usable BLE-MIDI instrument, controller, module or adapter.

## GUI status

A musician-facing GTK/libadwaita GUI is under active development and is planned for version 1.0. The GUI is intended to provide a simple musician workflow: scan for BLE-MIDI devices, select the target instrument or controller, connect it, and keep advanced telemetry behind the settings/details controls.

![midi-ble-rt GUI in development](docs/images/gui-main.jpg)

The GUI is not required to use the project today. The daemon and CLI are already usable and remain the primary validated interface while the GUI is developed.

## Installed commands

- `midi-ble-rtd`
- `midi-ble-rtctl`

Version commands:

- `midi-ble-rtd -v`
- `midi-ble-rtd -vv`
- `midi-ble-rtctl -v`
- `midi-ble-rtctl -vv`

## Data path

```text
BLE-MIDI device <-> BlueZ/GATT <-> midi-ble-rtd <-> ALSA Sequencer <-> MIDI apps
```

## Configuration

Current runtime uses a configuration directory:

```text
~/.config/midi-ble-rt/
├── daemon.ini
└── devices.d/
    ├── roland-gokeys.ini
    └── standard-ble-midi.ini
```

Run the daemon with the configuration directory:

```text
midi-ble-rtd --config ~/.config/midi-ble-rt
```

Expected startup marker:

```text
midi-ble-rtd
Runtime: config-directory multi-session
```

The legacy single-file runtime path is not the current validation path.

## Documentation map

- Generic device tutorial: `docs/GENERIC_BLE_MIDI_DEVICE.md`
- Developer notes and test internals: `DEVELOPERS.md`
- Architecture: `docs/ARCHITECTURE.md`
- Multi-device directory config: `docs/MULTI_DEVICE_CONFIG.md`
- Reconnect and timeout behavior: `docs/SESSION_RESILIENCE.md`
- Latency and RtKit evaluation: `docs/LATENCY_TESTING.md`
- Roland GO:KEYS validation: `docs/GOKEYS_TESTS.md`
- Release notes: `RELEASE_NOTES_v0.9.2.md`

## RtKit default policy

RtKit scheduling is optional and disabled globally by default.

```ini
[realtime]
rtkit = no
rt_priority = 1
realtime_rx = yes
realtime_tx = no
```

When RtKit is enabled, RX is selected by default because it improved the BLE-MIDI to ALSA path in validation. TX remains disabled by default because GO:KEYS `aplaymidi` testing showed worse TX worst-case latency and jitter when TX used realtime scheduling.

## Roland GO:KEYS operational rule

Connect MIDI first, then Audio/A2DP if needed. Audio mode is not the MIDI path. The daemon validates the MIDI target by BLE-MIDI GATT service and characteristic, not by BlueZ name or alias.
