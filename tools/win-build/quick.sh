#!/usr/bin/env bash
# Quick rebuild after a change in the Natron sources (C++, PyPlugs, themes,
# templates): only phase 04 (incremental make: recompiles the changed files)
# and a light install that refreshes what can change, without re-copying the
# ~1 GB of DLLs, Python stdlib, OCIO configs and OFX plugins.
#
#   bash quick.sh            build Natron + refresh the install
#   bash quick.sh --no-build refresh the install only (PyPlugs / themes / templates edits)
#
# Use build-all.sh after a git pull that touches tools/win-build (new
# patches, dependencies), the OFX plugins (openfx-misc...) or Cycles.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; source "$HERE/config.sh"; source "$HERE/lib.sh"
require_mingw

B="$NATRON_ROOT/Natron/build-qt6"
I="$INSTALL_DIR"
SRC="$NATRON_ROOT/Natron"
[ -f "$I/bin/Natron.exe" ] || die "no install at $I yet: run build-all.sh once first"

START=$(date +%s)
if [ "${1:-}" != "--no-build" ]; then
  mkdir -p "$HERE/logs"
  bash "$HERE/04-natron.sh" 2>&1 | tee "$HERE/logs/04-natron.log"
fi

log "Refreshing the install (binaries, PyPlugs, themes, templates)"
cp "$B/App/Natron.exe" "$I/bin/"
[ -f "$B/Renderer/NatronRenderer.exe" ] && cp "$B/Renderer/NatronRenderer.exe" "$I/bin/"
if [ "${STRIP_INSTALL:-1}" = "1" ]; then
  strip --strip-debug "$I/bin/Natron.exe" "$I/bin/NatronRenderer.exe" 2>/dev/null || true
fi
[ -d "$B/Plugins/PyPlugs" ] && cp -r "$B/Plugins/PyPlugs/." "$I/Plugins/PyPlugs/"
[ -d "$SRC/Gui/Resources/PyPlugs/3DE_Lens_Import" ] && cp -r "$SRC/Gui/Resources/PyPlugs/3DE_Lens_Import" "$I/Plugins/PyPlugs/"
[ -d "$SRC/Gui/Resources/PyPlugs/R3D_Import" ] && cp -r "$SRC/Gui/Resources/PyPlugs/R3D_Import" "$I/Plugins/PyPlugs/"
[ -d "$SRC/Gui/Resources/PyPlugs/CameraRAW_Import" ] && cp -r "$SRC/Gui/Resources/PyPlugs/CameraRAW_Import" "$I/Plugins/PyPlugs/"
rm -rf "$I/Plugins/PyPlugs/CameraRAW_Import/__pycache__"
mkdir -p "$I/Themes" "$I/Resources/Templates"
[ -d "$SRC/Gui/Resources/Themes" ]    && cp -r "$SRC/Gui/Resources/Themes/."    "$I/Themes/"
[ -d "$SRC/Gui/Resources/Templates" ] && cp -r "$SRC/Gui/Resources/Templates/." "$I/Resources/Templates/"
[ -f "$SRC/libs/ldpk/LICENSE.txt" ] && mkdir -p "$I/Licenses" && cp "$SRC/libs/ldpk/LICENSE.txt" "$I/Licenses/ldpk-LICENSE.txt"
for drop in Channel/Shuffle_N Nuketron_Theme; do rm -rf "$I/Plugins/PyPlugs/$drop"; done

ok "Quick rebuild done in $(( $(date +%s) - START ))s. Launch: $I/bin/Natron.exe"
