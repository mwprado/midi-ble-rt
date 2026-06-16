# SELinux notes

SELinux must be treated as part of the integration, not as an afterthought.

There are two distinct SELinux cases:

```text
Case A — BlueZ native MIDI profile
  bluetoothd runs under the Bluetooth SELinux domain.
  It may be denied access to ALSA Sequencer (/dev/snd/seq).
  Symptom: the BlueZ-created ALSA MIDI port does not appear or does not work.

Case B — midi-ble-rtd daemon
  midi-ble-rtd creates the ALSA Sequencer port itself.
  If it runs as an unconfined user process, SELinux may not block it.
  If it runs as a system service under a confined domain, it needs its own policy.
```

## Never use global SELinux disablement as the fix

Do not use this as the permanent solution:

```bash
sudo setenforce 0
```

Use it only as a short diagnostic. Prefer isolating the affected domain.

## Capture the denial

Run the failing test with SELinux enforcing:

```bash
getenforce
sudo ausearch -m avc,user_avc -ts recent
```

For BlueZ:

```bash
sudo ausearch -c bluetoothd -m avc,user_avc -ts recent
```

For this daemon:

```bash
sudo ausearch -c midi-ble-rtd -m avc,user_avc -ts recent
```

Inspect contexts:

```bash
ps -eZ | grep -E 'bluetoothd|midi-ble-rtd'
ls -Z /dev/snd/seq
```

Expected target for ALSA Sequencer is usually `/dev/snd/seq`, often labeled as a sound device type. The exact type must be taken from `ls -Z /dev/snd/seq` and the AVC log on the target Fedora system.

## Isolate BlueZ policy issue

If the problem is the native BlueZ MIDI profile, test only the Bluetooth SELinux domain in permissive mode:

```bash
sudo semanage permissive -a bluetooth_t
```

Then retry the MIDI connection. If it works, the denial is in the BlueZ/Bluetooth domain.

Undo:

```bash
sudo semanage permissive -d bluetooth_t
```

This is a diagnostic step, not the final policy.

## Generate a local policy from real AVCs

After reproducing the failure:

```bash
sudo ausearch -c bluetoothd -m avc,user_avc -ts recent --raw | audit2allow -M bluez_midi_alsa
cat bluez_midi_alsa.te
sudo semodule -i bluez_midi_alsa.pp
```

For the daemon:

```bash
sudo ausearch -c midi-ble-rtd -m avc,user_avc -ts recent --raw | audit2allow -M midi_ble_rtd_local
cat midi_ble_rtd_local.te
sudo semodule -i midi_ble_rtd_local.pp
```

Review the `.te` file before installing it. Avoid accepting broad rules that grant more than ALSA Sequencer and D-Bus access required by the daemon.

## Expected minimal BlueZ/ALSA shape

A BlueZ native MIDI-profile denial may look conceptually like:

```text
scontext=...:bluetooth_t:...
tcontext=...:sound_device_t:...
tclass=chr_file
denied { open read write ioctl getattr }
```

A narrow local policy might therefore look like this, but verify the exact target type on your system:

```te
module bluez_midi_alsa 1.0;

require {
    type bluetooth_t;
    type sound_device_t;
    class chr_file { getattr open read write ioctl };
}

allow bluetooth_t sound_device_t:chr_file { getattr open read write ioctl };
```

This example is intentionally limited to character-device access for ALSA Sequencer/sound devices. Do not install it blindly if your AVC shows different types or permissions.

## Recommended project policy

For `midi-ble-rtd`, start with user-run mode:

```bash
./build/midi-ble-rtd --config ~/.config/midi-ble-rt/roland-gokeys.ini
```

Once stable, package it as a system service and add a dedicated SELinux policy. The dedicated policy should permit:

```text
midi-ble-rtd -> system D-Bus / BlueZ D-Bus calls
midi-ble-rtd -> /dev/snd/seq read/write/ioctl
midi-ble-rtd -> config file read
```

The exact types must come from the local Fedora policy and AVC logs.
