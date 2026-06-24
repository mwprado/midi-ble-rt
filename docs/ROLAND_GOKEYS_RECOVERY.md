# Roland GO:KEYS RX recovery

This note documents an observed Roland GO:KEYS / BlueZ recovery case.

## Symptom

The transmit path works, but receive is silent:

```text
aplaymidi -p CLIENT:PORT ...   works
aseqdump -p CLIENT:PORT        waits forever
```

In this state, the ALSA Sequencer port and the GATT write path are alive, but the BlueZ `StartNotify` / indication session may not be delivering `Value` changes from the Roland BLE-MIDI characteristic.

Observed characteristic flags on Roland GO:KEYS:

```text
read write-without-response indicate
```

The daemon must treat either `notify` or `indicate` as a valid RX capability, then call `StartNotify()` on the persistent daemon D-Bus connection.

## Recovery sequence

For a user service:

```bash
systemctl --user stop midi-ble-rtd
sudo systemctl restart bluetooth
systemctl --user start midi-ble-rtd
```

For a system service:

```bash
sudo systemctl stop midi-ble-rtd
sudo systemctl restart bluetooth
sudo systemctl start midi-ble-rtd
```

If `Device1.Connect()` times out even though the device is paired and trusted, power-cycle the GO:KEYS, then connect MIDI/BLE before Audio/A2DP.

## Interpretation

This is not a MIDI parser or ALSA routing failure when `aplaymidi` works. It is a stale BlueZ/GATT/session state affecting RX delivery.

The clean restart forces:

```text
BlueZ object/GATT state reset
GO:KEYS service/characteristic rediscovery
daemon D-Bus connection recreation
StartNotify reactivation
```

After recovery, validate both directions:

```bash
aconnect -l
aseqdump -p CLIENT:PORT
aplaymidi -p CLIENT:PORT examples/midi/test-note.mid
```
