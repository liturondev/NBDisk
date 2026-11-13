# Prints the version to package: the release workflow sets NBDISK_VERSION from
# the pushed tag, while branch builds fall back to the version in CMakeLists.txt.
nbdisk_version() {
    version=${NBDISK_VERSION:-}
    if [ -z "$version" ]; then
        version=$(sed -n 's/^set(NBDISK_VERSION "\([0-9][0-9.]*\)".*/\1/p' CMakeLists.txt)
    fi
    version=${version#v}
    if [ -z "$version" ]; then
        echo "Could not determine the NBDisk version" >&2
        return 1
    fi
    printf '%s\n' "$version"
}
