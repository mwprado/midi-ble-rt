Name:           midi-ble-rt
Version:        0.1.0
Release:        0.1%{?dist}
Summary:        BLE-MIDI/GATT to ALSA Sequencer bridge

# FIXME: replace with the final upstream SPDX license once LICENSE is added.
# COPR will build with this placeholder, but Fedora review will require a real license.
License:        LicenseRef-Unknown
URL:            https://github.com/mwprado/midi-ble-rt
Source0:        %{name}-%{version}.tar.gz

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
%cmake
%cmake_build

%install
%cmake_install

install -D -m 0644 packaging/fedora/midi-ble-rtd.service \
    %{buildroot}%{_userunitdir}/midi-ble-rtd.service

install -D -m 0644 examples/midi/test-note.mid \
    %{buildroot}%{_datadir}/%{name}/examples/midi/test-note.mid
install -D -m 0644 examples/midi/README.md \
    %{buildroot}%{_datadir}/%{name}/examples/midi/README.md

install -D -m 0755 scripts/test-alsa-loopback.sh \
    %{buildroot}%{_datadir}/%{name}/scripts/test-alsa-loopback.sh
install -D -m 0755 scripts/test-fluidsynth-smoke.sh \
    %{buildroot}%{_datadir}/%{name}/scripts/test-fluidsynth-smoke.sh

%post
%systemd_user_post midi-ble-rtd.service

%preun
%systemd_user_preun midi-ble-rtd.service

%postun
%systemd_user_postun_with_restart midi-ble-rtd.service

%files
%doc README.md docs/*.md
%{_bindir}/midi-ble-rtd
%{_bindir}/midi-ble-rtctl
%{_datadir}/%{name}/
%{_userunitdir}/midi-ble-rtd.service

%changelog
* Mon Jun 22 2026 Moacyr Prado <mwprado@gmail.com> - 0.1.0-0.1
- Initial COPR-oriented Fedora package.
- Package BLE-MIDI daemon, control CLI, user systemd unit and MIDI smoke tests.
