# SELinux policy examples

These files are examples. Prefer generating local modules from real AVC logs:

```bash
sudo ausearch -c bluetoothd -m avc,user_avc -ts recent --raw | audit2allow -M bluez_midi_alsa
cat bluez_midi_alsa.te
sudo semodule -i bluez_midi_alsa.pp
```

For `midi-ble-rtd`, use:

```bash
sudo ausearch -c midi-ble-rtd -m avc,user_avc -ts recent --raw | audit2allow -M midi_ble_rtd_local
cat midi_ble_rtd_local.te
sudo semodule -i midi_ble_rtd_local.pp
```
