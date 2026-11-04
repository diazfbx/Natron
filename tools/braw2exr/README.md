# braw2exr

Small command-line tool that converts a Blackmagic RAW clip (`.braw`) to an
OpenEXR sequence (RGB, 16-bit half or 32-bit float, uncompressed). The Natron
fork's **File > Import BRAW (Blackmagic)...** and Read nodes given a
`.braw` call it, then load the EXR.

Natron itself never links Blackmagic code: braw2exr is a separate program
(MIT licence), built on your machine against Blackmagic Design's **free
Blackmagic RAW SDK**, which is **not** in this repository.

## Build (Windows, MSYS2 UCRT64)

1. Install the Blackmagic RAW SDK (it also installs the Blackmagic RAW
   runtime): <https://www.blackmagicdesign.com/developer/products/braw>
2. In the MSYS2 UCRT64 terminal:

```bash
cd /s/Natron
bash tools/braw2exr/build.sh
# or, SDK / Natron elsewhere:
bash tools/braw2exr/build.sh "/c/path/to/Blackmagic RAW SDK/Win" /s/Natron-install
```

`braw2exr.exe` is copied to `S:/Natron-install/bin`, where Natron looks for it
first. Otherwise Natron asks for its location once.

## Usage

```
braw2exr --list clip.braw
braw2exr --input clip.braw --output out/clip.%04d.exr --first-number 1001 \
         [--start 0] [--count 0] [--scale full|half|quarter|eighth] \
         [--gamma "<name>"] [--gamut "<name>"] [--float]
```

`--list` prints the clip size, frame count, frame rate and the gamma / gamut
names the SDK offers for that clip (they depend on the camera and SDK
version); Natron's import dialog fills its colour menus from it. Without
`--gamma` / `--gamut` the clip's own settings (camera / sidecar) are used.

Status: written against the SDK documentation, not yet compiled against the
real SDK on every platform. If the build fails, the compiler message names the
SDK call to adjust.
