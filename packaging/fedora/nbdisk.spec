# Portable spec file for Fedora and openSUSE Tumbleweed. The release workflow
# passes the tag-derived version with --define; the default below is only for
# local builds.

# Ship the application package only: rpm would otherwise build debuginfo and
# debugsource subpackages.
%global debug_package %{nil}

%{!?_nbdisk_version:%global _nbdisk_version 0.1.0}
%{!?_unitdir:%global _unitdir %{_prefix}/lib/systemd/system}
%{!?_sysusersdir:%global _sysusersdir %{_prefix}/lib/sysusers.d}

Name:           nbdisk
Version:        %{_nbdisk_version}
Release:        1%{?dist}
Summary:        Qt/KDE client for connecting and mounting remote NBD disks
License:        GPL-3.0-or-later
URL:            https://github.com/liturondev/NBDisk
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  systemd-rpm-macros
%if 0%{?suse_version}
BuildRequires:  qt6-base-devel
%else
BuildRequires:  qt6-qtbase-devel
%endif
BuildRequires:  cmake(KF6Config)
BuildRequires:  cmake(KF6ConfigWidgets)
BuildRequires:  cmake(KF6CoreAddons)
BuildRequires:  cmake(KF6I18n)
BuildRequires:  cmake(KF6Wallet)
BuildRequires:  cmake(KF6WidgetsAddons)
BuildRequires:  cmake(KF6XmlGui)

Requires:       hicolor-icon-theme
Requires:       udisks2
Requires:       polkit
Requires:       kmod
Requires(post):   systemd
Requires(preun):  systemd
Requires(postun): systemd
Recommends:     nbd
Recommends:     qemu-img
Recommends:     cryptsetup
Recommends:     polkit-kde-agent-1

%description
NBDisk is a Qt 6 / KDE Frameworks 6 desktop client that connects remote
Network Block Device (NBD) exports to local /dev/nbd devices and mounts the
resulting volumes.

It lists every NBD device the kernel exposes, connects and disconnects exports
with nbd-client or qemu-nbd, shows capacity and volume state, and mounts or
unmounts volumes through udisks or configured mount points. LUKS-encrypted
volumes can be unlocked with a passphrase remembered in KDE Wallet, connection
profiles reconnect at login through the bundled background service, and an
optional privileged helper avoids repeated polkit prompts.

%prep
%autosetup -n %{name}-%{version}

%build
%{__cmake} -S . -B build \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX=%{_prefix} \
    -DNBDISK_VERSION=%{version} \
    -DBUILD_TESTING=ON
%{__cmake} --build build --parallel

%check
ctest --test-dir build --output-on-failure

%install
DESTDIR=%{buildroot} %{__cmake} --install build
install -Dpm644 LICENSE %{buildroot}%{_datadir}/licenses/%{name}/LICENSE
%if 0%{?suse_version}
# openSUSE skips its automatic stripping when no debug package is produced.
find %{buildroot}%{_bindir} -type f -exec %{__strip} --strip-unneeded {} +
%endif

%post
%systemd_post nbdisk-helper.service

%preun
%systemd_preun nbdisk-helper.service

%postun
%systemd_postun_with_restart nbdisk-helper.service

%files
%{_bindir}/nbdisk
%{_bindir}/nbdisk-daemon
%{_bindir}/nbdisk-helper
%{_unitdir}/nbdisk-helper.service
%{_sysusersdir}/nbdisk.conf
%{_datadir}/applications/io.github.liturondev.NBDisk.desktop
%{_datadir}/metainfo/io.github.liturondev.NBDisk.metainfo.xml
%{_datadir}/icons/hicolor/scalable/apps/io.github.liturondev.NBDisk.svg
%{_datadir}/nbdisk/nbdiskui.rc
%license %{_datadir}/licenses/%{name}/LICENSE
%doc README.md

%changelog
* Fri Nov 13 2026 liturondev <339808872+liturondev@users.noreply.github.com> - 0.1.0-1
- Initial package.
