Name:           midi-ble-rt
Version:        0.6.0
Release:        1%{?dist}
Summary:        BLE-MIDI/GATT to ALSA Sequencer bridge

License:        MIT
URL:            https://github.com/mwprado/midi-ble-rt
Source0:        %{url}/archive/refs/tags/v%{version}.tar.gz#/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  cmake
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

The first validated target is the Roland GO:KEYS family.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -DBUILD_SHARED_LIBS=OFF
%cmake_build

%install
%cmake_install

%files
%license LICENSE
%doc README.md DEVELOPERS.md RELEASE_NOTES_v*.md docs/*.md
%{_bindir}/midi-ble-rtd
%{_bindir}/midi-ble-rtctl
%{_datadir}/%{name}/
%{_userunitdir}/midi-ble-rtd.service

%changelog
* Fri Jun 26 2026 Moacyr Prado <mwprado@users.noreply.github.com> - 0.6.0-1
- Include internal code cleanup after the stable RPM release.
- Extract BlueZ, BLE-MIDI GATT and ALSA port helper modules.
- Add stats v3 with separate ALSA RX and ALSA TX fields.
- Update midi-ble-rtctl stats/top to display ALSA RX and ALSA TX separately.
- Preserve existing BLE-MIDI runtime behavior.

* Fri Jun 26 2026 Moacyr Prado <mwprado@users.noreply.github.com> - 0.5.2-1
- Add stats v3 with separate ALSA RX and ALSA TX fields.
- Update midi-ble-rtctl stats/top to display ALSA RX and ALSA TX separately.
- Include code cleanup: BlueZ/GATT/ALSA helper extraction and legacy boundary reduction.
- Preserve existing BLE-MIDI runtime behavior.

* Thu Jun 25 2026 Moacyr Prado <mwprado@gmail.com> - 0.5.1-2
- Build the internal midi-ble-rt-core library statically.
- Avoid a broken runtime dependency on libmidi-ble-rt-core.so()(64bit).

* Thu Jun 25 2026 Moacyr Prado <mwprado@gmail.com> - 0.5.0-1
- Add session statistics monitoring.
- Export runtime statistics to stats.tsv under the user runtime directory.
- Add midi-ble-rtctl stats and midi-ble-rtctl top.
- Document the statistics format and operational semantics.

* Wed Jun 24 2026 Moacyr Prado <mwprado@gmail.com> - 0.4.0-1
- Use single public midi-ble-rtd daemon with internal orchestrator runtime.
- Stop building the experimental midi-ble-rtd-duplex target.
- Add mb-alsa core helpers and tests for ALSA event classification.
- Update architecture documentation for daemon, orchestrator and core split.

* Tue Jun 23 2026 Moacyr Prado <mwprado@gmail.com> - 0.2.0-1
- Add MIDI session state core and daemon session registry.
- Add unit tests for state transitions, multi-session isolation and address identity.
- Document developer architecture and session state diagrams.
- Keep BlueZ as Bluetooth/GATT source of truth and ALSA as MIDI endpoint.

* Mon Jun 22 2026 Moacyr Prado <mwprado@gmail.com> - 0.1.1-1
- Consolidate Fedora spec under packaging/fedora.
- Do not install missing examples/midi/README.md.

* Mon Jun 22 2026 Moacyr Prado <mwprado@gmail.com> - 0.1.0-0.3
- Point Source0 to GitHub tag tarball for COPR SCM/spec builds.

* Mon Jun 22 2026 Moacyr Prado <mwprado@gmail.com> - 0.1.0-0.2
- Switch package license to MIT and install LICENSE as %license.

* Mon Jun 22 2026 Moacyr Prado <mwprado@gmail.com> - 0.1.0-0.1
- Initial COPR-oriented Fedora package.
- Package BLE-MIDI daemon, control CLI, user systemd unit and MIDI smoke tests.
