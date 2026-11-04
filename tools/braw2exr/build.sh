#!/usr/bin/env bash
# Build braw2exr in MSYS2 (UCRT64) and copy it next to Natron.exe.
#   bash tools/braw2exr/build.sh ["<Blackmagic RAW SDK>/Win"] [<Natron install dir>]
# Needs the Blackmagic RAW SDK (free, from blackmagicdesign.com) - not in this repo.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="${1:-${BRAW_SDK_DIR:-/c/Program Files (x86)/Blackmagic Design/Blackmagic RAW/Blackmagic RAW SDK/Win}}"
INSTALL="${2:-/s/Natron-install}"
BUILD="$HERE/build"
cmake -S "$HERE" -B "$BUILD" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DBRAW_SDK_DIR="$SDK"
cmake --build "$BUILD" -j4
EXE="$BUILD/braw2exr.exe"; [ -f "$EXE" ] || EXE="$BUILD/braw2exr"
if [ -d "$INSTALL/bin" ]; then
    cp "$EXE" "$INSTALL/bin/"
    echo "braw2exr copied to $INSTALL/bin (the BRAW import finds it there)"
else
    echo "Built: $EXE (copy it next to Natron.exe, or pick it when Natron asks)"
fi
