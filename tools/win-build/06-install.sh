#!/usr/bin/env bash
# Phase 06 — assemble a clean, relocatable install into $INSTALL_DIR.
# Layout = the release archives' (and what Natron resolves relative to its binary):
#   bin/Natron.exe + bin/NatronRenderer.exe + ONE set of DLLs + platforms/ + imageformats/ ;
#   lib/pythonX.Y ;  Plugins/ ;  Resources/ ;  etc/
# (Earlier versions of this script staged App/ and Renderer/ with two copies of the
# DLLs, ~1.3 GB each; those folders are removed when found.)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; source "$HERE/config.sh"; source "$HERE/lib.sh"
require_mingw

B="$NATRON_ROOT/Natron/build-qt6"
I="$INSTALL_DIR"
PYV="$(py_ver)"   # e.g. 3.14
[ -f "$B/App/Natron.exe" ] || die "build output missing ($B/App/Natron.exe) — run 04-natron.sh first"

log "Staging install into $I (python $PYV)"
# Older App/ + Renderer/ layout from previous runs: replaced by bin/.
for old in App Renderer; do
  if [ -d "$I/$old" ]; then rm -rf "$I/$old"; ok "removed the old $old/ folder (now bin/)"; fi
done
BIN="$I/bin"
mkdir -p "$BIN/platforms" "$I/lib" \
         "$I/Plugins/OFX/Natron" "$I/Plugins/PyPlugs" "$I/Resources"

# OIDN loads its CPU device module at runtime by the UN-prefixed name, but
# MSYS2 ships it lib-prefixed — without this copy, Cycles denoise fails with
# "unsupported device type: CPU".
fix_oidn() { # dir
  if [ ! -f "$1/OpenImageDenoise_device_cpu.dll" ] && [ -f "$1/libOpenImageDenoise_device_cpu.dll" ]; then
    cp "$1/libOpenImageDenoise_device_cpu.dll" "$1/OpenImageDenoise_device_cpu.dll"
    ok "$(basename "$1"): staged un-prefixed OIDN CPU device module"
  fi
}

# --- GUI binary + bundled DLLs + Qt platform plugin ---
log "bin: Natron.exe + DLLs"
cp "$B/App/Natron.exe" "$BIN/"
cp "$MINGW"/bin/*.dll "$BIN/"
fix_oidn "$BIN"
cp "$MINGW/share/qt6/plugins/platforms/qwindows.dll" "$BIN/platforms/"
# Qt image-format plugins: Qt has only PNG/BMP/PPM built in; JPEG, TIFF, WebP, SVG,
# GIF, ICO... are plugins. Natron itself reads images through OpenImageIO, but
# PySide6 scripts and PyPlugs using QImage/QPixmap had no JPEG writer in the
# portable (user report 2026-10-04). The Linux AppImage already bundles the set.
# Their extra DLLs (libmng-2, Qt6Svg) come with the bin/*.dll sweep above.
mkdir -p "$BIN/imageformats"
cp "$MINGW"/share/qt6/plugins/imageformats/*.dll "$BIN/imageformats/"

# --- Headless renderer (optional): same folder, shares the DLLs ---
if [ "${STAGE_RENDERER:-1}" = "1" ]; then
  log "bin: NatronRenderer.exe"
  cp "$B/Renderer/NatronRenderer.exe" "$BIN/"
fi

# FastVolumeRender runtime: wgpu-native isn't an MSYS2 package, so the DLL
# must come from the build's wgpu dir (only present when the node was built).
if [ -f "$NATRON_ROOT/wgpu/wgpu_native.dll" ]; then
  cp "$NATRON_ROOT/wgpu/wgpu_native.dll" "$BIN/"
  ok "staged wgpu_native.dll (FastVolumeRender)"
fi

# Cycles GPU kernels (WITH_CUDA=1): Cycles loads them from lib/ next to the exe.
KDIR="$NATRON_ROOT/cycles/build/src/kernel"
if [ "${WITH_CUDA:-0}" = "1" ] && ls "$KDIR"/kernel_*.zst >/dev/null 2>&1; then
  mkdir -p "$BIN/lib"
  cp "$KDIR"/kernel_*.zst "$BIN/lib/"
  ok "staged $(ls "$KDIR"/kernel_*.zst | wc -l) Cycles GPU kernels (bin/lib)"
fi

# --- Python standard library (+ PySide6, qtpy live in its site-packages) ---
log "Python stdlib (lib/python$PYV)"
rm -rf "$I/lib/python$PYV"
cp -r "$MINGW/lib/python$PYV" "$I/lib/python$PYV"

# --- OFX plugins into .ofx.bundle/Contents/Win64 ---
log "OFX plugins (Misc, CImg, IO, Arena)"
install_ofx() { # name srcpath
  local n="$1" src="$2" d="$I/Plugins/OFX/Natron/$1.ofx.bundle/Contents/Win64"
  mkdir -p "$d"; cp "$src" "$d/$1.ofx"
}
install_ofx Misc "$NATRON_ROOT/openfx-misc/build/Misc.ofx"
install_ofx CImg "$NATRON_ROOT/openfx-misc/build/CImg.ofx"
install_ofx IO   "$NATRON_ROOT/openfx-io/build/IO.ofx"
install_ofx Arena "$NATRON_ROOT/openfx-arena/build/Arena.ofx"
install_ofx NikKit "$NATRON_ROOT/nik-kit-natron/build/NikKit.ofx"
# Arena needs ImageMagick's config files (delegates, colors, policy) at runtime;
# the DLLs come with the DLL sweep above; MSYS2 ImageMagick resolves <dll dir>/../etc.
if [ -d "$MINGW/etc/ImageMagick-7" ]; then
  mkdir -p "$I/etc"
  cp -r "$MINGW/etc/ImageMagick-7" "$I/etc/"
fi

# --- PyPlugs: built-ins (from the build's POST_BUILD) + community pack ---
log "PyPlugs (built-in + community)"
[ -d "$B/Plugins/PyPlugs" ] && cp -r "$B/Plugins/PyPlugs/." "$I/Plugins/PyPlugs/" || warn "no built-in PyPlugs found in build tree"
cp -r "$NATRON_ROOT/natron-plugins/." "$I/Plugins/PyPlugs/"
# nik-kit-natron's PyPlugs (menu group "Nik Kit") live in their own folder
if ls "$NATRON_ROOT/nik-kit-natron/pyplugs"/*/ >/dev/null 2>&1; then
  mkdir -p "$I/Plugins/PyPlugs/NikKit"
  cp -r "$NATRON_ROOT/nik-kit-natron/pyplugs"/*/ "$I/Plugins/PyPlugs/NikKit/"
fi
# Drop the upstream .git metadata — it shouldn't ship in a deployable
# bundle (and adds ~100 MB of dead weight).
rm -rf "$I/Plugins/PyPlugs/.git"

# --- Licences: Natron's GPL + the vendored MIT libraries ReadGeo links (cgltf, ufbx) ---
# MIT requires the licence text to accompany binary distributions.
log "Licenses"
mkdir -p "$I/Licenses"
cp "$NATRON_ROOT/Natron/LICENSE.txt" "$I/Licenses/Natron-LICENSE.txt"
cp "$NATRON_ROOT/Natron/libs/cgltf/LICENSE" "$I/Licenses/cgltf-LICENSE.txt"
cp "$NATRON_ROOT/Natron/libs/ufbx/LICENSE" "$I/Licenses/ufbx-LICENSE.txt"

# --- OpenColorIO configs: reuse local copy if present, else download (non-fatal) ---
if [ -d "$I/Resources/OpenColorIO-Configs" ]; then
  ok "OCIO configs already present"
elif [ -d "$B/Resources/OpenColorIO-Configs" ]; then
  log "OCIO configs (reusing local copy)"; cp -r "$B/Resources/OpenColorIO-Configs" "$I/Resources/OpenColorIO-Configs"
else
  log "OCIO configs (downloading Natron-v2.4)"
  if ( cd "$I/Resources" \
       && curl -L -s -S https://github.com/NatronGitHub/OpenColorIO-Configs/archive/Natron-v2.4.tar.gz -o ocio.tgz \
       && tar xzf ocio.tgz && mv OpenColorIO-Configs-Natron-v2.4 OpenColorIO-Configs && rm -f ocio.tgz ); then
    ok "OCIO configs installed"
  else
    warn "OCIO download failed — Natron still runs (color presets unavailable). Re-run 06 to retry."
  fi
fi

# --- Modern ACES configs (ACES 2.1 / 1.3, CG + Studio) from tools/ocio-configs ---
# The pack above only has pre-1.0 ACES; these are OpenColorIO's official built-in
# configs, self-contained. Copied every run so an existing install gets them too.
OCIO_EXTRA="$NATRON_ROOT/Natron/tools/ocio-configs"
if [ -d "$OCIO_EXTRA" ]; then
  log "OCIO configs: modern ACES (tools/ocio-configs)"
  mkdir -p "$I/Resources/OpenColorIO-Configs"
  for d in "$OCIO_EXTRA"/*/; do
    [ -f "$d/config.ocio" ] || continue
    rm -rf "$I/Resources/OpenColorIO-Configs/$(basename "$d")"
    cp -r "$d" "$I/Resources/OpenColorIO-Configs/"
    ok "  $(basename "$d")"
  done
  cp "$OCIO_EXTRA/LICENSE-OpenColorIO.txt" "$I/Licenses/OpenColorIO-configs-LICENSE.txt"
fi

# --- Themes (stylesheets) at the install root: <Natron>/Themes/<name>/<name>.qss ---
# The Themes menu PyPlug (Nuketron_Theme) comes with the PyPlugs copy above.
THEMES_SRC="$NATRON_ROOT/Natron/Gui/Resources/Themes"
if [ -d "$THEMES_SRC" ]; then
  log "Themes (Gui/Resources/Themes)"
  mkdir -p "$I/Themes"; cp -r "$THEMES_SRC/." "$I/Themes/"   # keeps themes you added yourself
  ok "  $(ls -d "$I/Themes"/*/ 2>/dev/null | xargs -n1 basename | tr '\n' ' ')"
fi
if [ ! -f "$I/Plugins/PyPlugs/Nuketron_Theme/initGui.py" ] && [ -d "$NATRON_ROOT/Natron/Gui/Resources/PyPlugs/Nuketron_Theme" ]; then
  cp -r "$NATRON_ROOT/Natron/Gui/Resources/PyPlugs/Nuketron_Theme" "$I/Plugins/PyPlugs/"
fi

# --- Strip debug symbols from the staged binaries (build-qt6 keeps them) ---
if [ "${STRIP_INSTALL:-1}" = "1" ]; then
  log "Stripping debug symbols from the staged binaries (STRIP_INSTALL=1)"
  for f in "$BIN/Natron.exe" "$BIN/NatronRenderer.exe" $(find "$I/Plugins/OFX" -name '*.ofx' 2>/dev/null); do
    [ -f "$f" ] || continue
    before=$(du -m "$f" | cut -f1)
    strip --strip-debug "$f" && ok "  $(basename "$f"): ${before} MB -> $(du -m "$f" | cut -f1) MB" \
      || warn "  could not strip $f (kept as is)"
  done
fi

ok "Phase 06 (install) complete — staged at $I"
