#!/usr/bin/env bash
# Phase 00 — install MSYS2 (UCRT64) dependencies (idempotent; --needed skips installed).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; source "$HERE/config.sh"; source "$HERE/lib.sh"
require_mingw --no-gcc   # the toolchain is installed below

log "pacman -Syu (refresh + core update)"
pacman -Syu --noconfirm || warn "pacman -Syu returned non-zero — if it asked to CLOSE THE TERMINAL, reopen UCRT64 and re-run ./00-deps.sh before continuing."

log "Installing core build + runtime dependencies"
pacman -S --needed --noconfirm \
  "$PKG"-toolchain "$PKG"-cmake git \
  "$PKG"-qt6-base "$PKG"-pyside6 "$PKG"-shiboken6 \
  "$PKG"-qt6-imageformats "$PKG"-qt6-svg \
  "$PKG"-seexpr \
  "$PKG"-python "$PKG"-python-qtpy "$PKG"-boost \
  "$PKG"-cairo "$PKG"-expat "$PKG"-openvdb \
  "$PKG"-alembic \
  "$PKG"-openimageio "$PKG"-openexr "$PKG"-opencolorio \
  "$PKG"-ffmpeg "$PKG"-libraw "$PKG"-libpng \
  "$PKG"-imagemagick "$PKG"-libzip "$PKG"-poppler \
  "$PKG"-librevenge "$PKG"-libcdr "$PKG"-librsvg \
  "$PKG"-pango "$PKG"-lcms2 "$PKG"-fontconfig

if [ "${WITH_CYCLES:-1}" = "1" ]; then
  log "Installing Cycles dependencies"
  pacman -S --needed --noconfirm \
    "$PKG"-embree "$PKG"-openimagedenoise "$PKG"-openpgl \
    "$PKG"-opensubdiv "$PKG"-pugixml "$PKG"-libepoxy
fi
ok "Phase 00 (deps) complete"
