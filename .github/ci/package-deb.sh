#!/bin/bash
# Builds the Debian package inside a Debian or Ubuntu container.
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    build-essential cmake debhelper dpkg-dev fakeroot \
    qt6-base-dev \
    libkf6config-dev libkf6configwidgets-dev libkf6coreaddons-dev \
    libkf6i18n-dev libkf6wallet-dev libkf6widgetsaddons-dev libkf6xmlgui-dev

# Binary-only: the package is built from the checked-out tree.
dpkg-buildpackage -b -us -uc

mkdir -p dist
cp ../nbdisk_*.deb dist/
