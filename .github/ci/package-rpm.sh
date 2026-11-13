#!/bin/bash
# Builds the RPM package on Fedora or openSUSE Tumbleweed.
set -euo pipefail

version=$(sed -n 's/^project(NBDisk VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)
if [ -z "$version" ]; then
    echo "Could not read the version from CMakeLists.txt" >&2
    exit 1
fi

mkdir -p dist rpmbuild/BUILD rpmbuild/BUILDROOT rpmbuild/RPMS rpmbuild/SOURCES rpmbuild/SPECS rpmbuild/SRPMS

# git refuses to read a checkout owned by another uid inside containers.
git config --global --add safe.directory "$PWD"
git archive --prefix="nbdisk-$version/" -o "dist/nbdisk-$version.tar.gz" HEAD

rpmbuild -bb packaging/fedora/nbdisk.spec \
    --define "_topdir $PWD/rpmbuild" \
    --define "_sourcedir $PWD/dist" \
    --define "_nbdisk_version $version"

mkdir -p dist/rpms
find rpmbuild/RPMS -name '*.rpm' -exec cp {} dist/rpms/ \;
