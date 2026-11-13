#!/bin/bash
# Installs the build dependencies for one distribution. Used by the CI
# workflows and safe to run by hand inside the matching container image.
set -euo pipefail

distro=${1:?usage: install-deps.sh <arch|debian|ubuntu|fedora|opensuse>}

case "$distro" in
arch)
    pacman -Syu --noconfirm --needed \
        base-devel cmake ninja \
        qt6-base kconfigwidgets kcoreaddons ki18n kwallet kwidgetsaddons kxmlgui
    ;;
debian | ubuntu)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build \
        qt6-base-dev \
        libkf6config-dev libkf6configwidgets-dev libkf6coreaddons-dev \
        libkf6i18n-dev libkf6wallet-dev libkf6widgetsaddons-dev libkf6xmlgui-dev
    ;;
fedora)
    dnf install -y \
        cmake gcc-c++ make ninja-build \
        qt6-qtbase-devel \
        kf6-kconfig-devel kf6-kconfigwidgets-devel kf6-kcoreaddons-devel \
        kf6-ki18n-devel kf6-kwallet-devel kf6-kwidgetsaddons-devel kf6-kxmlgui-devel
    ;;
opensuse)
    # The image ships busybox-gawk, which conflicts with the gawk that
    # qt6-base-devel pulls in through cups-devel -> rpm-build.
    if rpm -q busybox-gawk >/dev/null 2>&1; then
        zypper --non-interactive remove busybox-gawk
    fi
    zypper --non-interactive install \
        cmake gcc-c++ make ninja \
        qt6-base-devel \
        kf6-kconfig-devel kf6-kconfigwidgets-devel kf6-kcoreaddons-devel \
        kf6-ki18n-devel kf6-kwallet-devel kf6-kwidgetsaddons-devel kf6-kxmlgui-devel
    ;;
*)
    echo "Unknown distribution: $distro" >&2
    exit 1
    ;;
esac
