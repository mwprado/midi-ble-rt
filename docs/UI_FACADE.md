# MIDI-BLE-RT UI façade

The graphical interface is split into a toolkit-independent UI façade and a
GNOME reference frontend.

```text
GUI frontend
    ↓
MIDI-BLE-RT UI façade
    ↓
current backend: midi-ble-rtctl + configuration files
    ↓
MIDI-BLE service
```

Rules:

- UI frontends must consume the façade.
- The GNOME frontend is the reference implementation.
- Frontends must not call `midi-ble-rtctl` directly.
- Frontends must not write device `.ini` files directly.
- Frontends must not access the control socket directly.
- Persisting a device is part of the connect flow.

Current façade responsibilities:

- get service/device snapshot
- scan/refresh visible devices
- connect a selected device
- disconnect a selected device
- refresh/recheck a selected device
- save device configuration under the daemon configuration directory

The current persistent device configuration target is:

```text
~/.config/midi-ble-rt/devices.d/<device-id>.ini
```

The GNOME frontend may use GTK/libadwaita, but `gui/facade/` must remain
independent from GTK and libadwaita.
