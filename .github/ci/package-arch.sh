#!/bin/bash
# Builds the Arch Linux package inside an archlinux:base-devel container.
set -euo pipefail

. .github/ci/version.sh
version=$(nbdisk_version)

.github/ci/install-deps.sh arch

# makepkg refuses to run as root.
id -u builder >/dev/null 2>&1 || useradd -m builder
chown -R builder:builder "$PWD"

su builder -c "cd '$PWD/packaging/arch' && NBDISK_VERSION='$version' makepkg --force --noconfirm --nodeps"

mkdir -p dist
cp packaging/arch/*.pkg.tar.zst dist/
