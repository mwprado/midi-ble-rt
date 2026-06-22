# systemd user service

`midi-ble-rtd` is designed to run well as a `systemd --user` service.

This is the preferred mode for the current project stage because:

- the daemon publishes an ALSA Sequencer port for the user's MIDI/audio session;
- it does not need root privileges;
- logs stay in the user's journal;
- the service can be restarted without touching system services.

Bluetooth pairing, trust and first connection are still explicit control-plane actions handled by `midi-ble-rtctl`.

## Install

Build first:

```bash
cmake -S . -B build
cmake --build build
```

Install the user service:

```bash
scripts/install-user-service.sh
```

Install and start immediately:

```bash
scripts/install-user-service.sh --start
```

The installer copies binaries to:

```text
~/.local/bin/midi-ble-rtd
~/.local/bin/midi-ble-rtctl
```

and writes:

```text
~/.config/systemd/user/midi-ble-rtd.service
```

Default config path:

```text
~/.config/midi-ble-rt/roland-gokeys.ini
```

## Prepare GO:KEYS

Before starting the service, prepare the device and config:

```bash
midi-ble-rtctl connect CB:81:F4:62:FF:07 --profile roland_gokeys --write-config
```

or, from the build tree:

```bash
./build/midi-ble-rtctl connect CB:81:F4:62:FF:07 --profile roland_gokeys --write-config
```

## Start and stop

```bash
systemctl --user start midi-ble-rtd.service
systemctl --user stop midi-ble-rtd.service
systemctl --user restart midi-ble-rtd.service
```

Enable at user login:

```bash
systemctl --user enable midi-ble-rtd.service
```

Disable:

```bash
systemctl --user disable midi-ble-rtd.service
```

## Logs

Follow logs:

```bash
journalctl --user -u midi-ble-rtd.service -f
```

Check status:

```bash
systemctl --user status midi-ble-rtd.service
```

## Verify ALSA port

After the service starts:

```bash
aconnect -l
aseqsend -l
aseqdump -l
```

Expected port name:

```text
midi-ble-rt Roland GO:KEYS BLE-MIDI
```

## Test transmit path

```bash
aplaymidi -p CLIENT:PORT examples/midi/test-note.mid
```

With debug enabled in the config, the service journal should show:

```text
ALSA->BLE MIDI: ...
BLE write: ...
```

## Linger

By default, a user service runs only while the user manager is active. For this project that is usually correct because the daemon is part of the interactive MIDI/audio session.

If the daemon should run before login or after logout, enable linger:

```bash
loginctl enable-linger "$USER"
```

This is optional and not recommended as the default during development.

## Why not a system service yet?

A system service is possible later, but it complicates ALSA Sequencer ownership, D-Bus authorization, desktop audio integration and per-user MIDI routing. For the current architecture, `systemd --user` is simpler and better aligned with ALSA/DAW usage.
