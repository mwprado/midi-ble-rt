Name:           midi-ble-rt
Version:        0.9.2
Release:        1%{?dist}
Summary:        BLE-MIDI/GATT to ALSA Sequencer bridge

License:        MIT
URL:            https://github.com/mwprado/midi-ble-rt
Source0:        %{url}/archive/refs/tags/v%{version}.tar.gz#/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  cmake
BuildRequires:  binutils
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(gio-2.0)
BuildRequires:  pkgconfig(alsa)
BuildRequires:  systemd-rpm-macros

Requires:       bluez
Recommends:     alsa-utils
Recommends:     fluidsynth

%description
midi-ble-rt is a Linux BLE-MIDI/GATT to ALSA Sequencer bridge.

It uses BlueZ as a generic BLE/GATT transport, subscribes to BLE-MIDI
notifications, exposes an ALSA Sequencer port, and can write MIDI messages back
to the BLE-MIDI device through GATT WriteValue.

The first validated hardware target is the Roland GO:KEYS family, but the
project target is any usable BLE-MIDI instrument, controller, module or adapter.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DMIDI_BLE_RT_VERSION=%{version} -DSYSTEMD_USER_UNIT_DIR=%{_userunitdir}
%cmake_build

%install
%cmake_install

%check
# midi-ble-rt-core is an internal implementation library. The installed
# executables must not depend on a private libmidi-ble-rt-core.so runtime object.
for bin in \
    %{buildroot}%{_bindir}/midi-ble-rtd \
    %{buildroot}%{_bindir}/midi-ble-rtctl
 do
    if readelf -d "$bin" | grep -q 'libmidi-ble-rt-core'; then
        echo "error: $bin has a runtime dependency on private libmidi-ble-rt-core" >&2
        exit 1
    fi
 done

%files
%license LICENSE
%doc README.md DEVELOPERS.md RELEASE_NOTES_v*.md docs/*.md
%{_docdir}/midi_ble_rt/MULTI_DEVICE_CONFIG.md
%{_bindir}/midi-ble-rtd
%{_bindir}/midi-ble-rtctl
%{_datadir}/%{name}/
%{_userunitdir}/midi-ble-rtd.service

%changelog
* Fri Jul 03 2026 Moacyr Prado <mwprado@users.noreply.github.com> - 0.9.2-1
- Add compiled version reporting with -v and -vv.
- Add optional RtKit scheduling through RealtimeKit.
- Allow realtime scheduling selection per RX and TX worker.
- Default realtime worker policy to RX enabled and TX disabled when RtKit is active.
- Add latency capture and comparison helper scripts.
- Document Roland GO:KEYS latency and RtKit validation results.

* Fri Jul 03 2026 Moacyr Prado <mwprado@users.noreply.github.com> - 0.9.1-1
- Add optional measurement-only latency diagnostics.
- Export latency.tsv separately from stats.tsv.
- Measure RX/TX queue latency and total daemon latency.
- Report count, average, p95, p99 and max latency metrics.
- Add midi-ble-rtctl latency and latency-top commands.
- Keep RtKit/realtime scheduling out of this release.

* Wed Jul 01 2026 Moacyr Prado <mwprado@users.noreply.github.com> - 0.9.0-1
- Add dataplane epoch fencing to prevent stale RX/TX after session changes.
- Drop pending RX/TX data when a device leaves STREAMING.
- Make the BLE-MIDI decoder stateful across packets.
- Handle fragmented Note On, running status, SysEx and realtime interleaving.
- Fail closed on ambiguous BLE-MIDI framing and emit MIDI panic on RX resync.
- Document BLE-MIDI fragmentation and resync policy.
- Remove the legacy single-file configuration path; keep directory-based multi-session runtime only.
