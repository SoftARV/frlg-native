#!/bin/sh
# Build the Flatpak and export a bundle.
#
# The stamp is why this exists rather than a bare flatpak-builder line. The
# manifest takes launcher/ as a directory source, so the build sees that
# directory and nothing above it -- no .git, and no way to read the commit it is
# building. A testing build that cannot say which commit it is is a bug report
# nobody can act on, so the commit is written to a file the build can read.
#
# The game is not built here: it needs agbcc, an arm-none-eabi toolchain and a
# byte-matching reference build (ADR 0021). Build it first with CMake:
#
#   cmake --build build/rom-play
set -e

root=$(git rev-parse --show-toplevel)
cd "$root"

game=build/rom-play/ports/desktop/frlg-native
if [ ! -x "$game" ]; then
    echo "no $game -- build it first: cmake --build build/rom-play" >&2
    exit 1
fi

# Read before the stamp is written, so the stamp cannot make the tree look dirty
# to itself. build-id.txt is ignored anyway; this is belt and braces.
id=$(git rev-parse --short HEAD)
if [ -n "$(git status --porcelain)" ]; then
    id="$id-dirty"
fi
when=$(git log -1 --format=%cs)

printf '%s\n%s\n' "$id" "$when" > launcher/build-id.txt
echo "stamped $id ($when)"

# Packaged as a Flatpak on some systems and installed on others.
if command -v flatpak-builder > /dev/null 2>&1; then
    builder="flatpak-builder"
else
    builder="flatpak run org.flatpak.Builder"
fi

rm -rf build/flatpak-build build/flatpak-repo build/frlg-native.flatpak

# rofiles-fuse needs a fuse mount, which is not always there to be had.
$builder --force-clean --disable-rofiles-fuse \
    --repo=build/flatpak-repo build/flatpak-build \
    ports/desktop/flatpak/io.github.softarv.frlg.yml

flatpak build-bundle build/flatpak-repo build/frlg-native.flatpak \
    io.github.softarv.frlg

echo
echo "bundle: $root/build/frlg-native.flatpak"
echo "install: flatpak install --user build/frlg-native.flatpak"
