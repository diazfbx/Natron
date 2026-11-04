# Natron fork — 3D / Cycles / Deep / Particles / Volumes

> ### About this fork (diazfbx/Natron)
>
> This repository is a personal fork of **[Niik-l/Natron](https://github.com/Niik-l/Natron)**, itself a fork of **[Natron](https://github.com/NatronGitHub/Natron)**. All the credit for Natron goes to its authors — Alexandre Gauthier-Foichat, INRIA and the Natron developers — and all the credit for the 3D system, Cycles, deep compositing, particles and everything described further down this page goes to **[Niik-l](https://github.com/Niik-l)**. The rest of this README is Niik-l's, kept as is.
>
> **Why this fork.** I really appreciate the approach of building pipelines on standalone, open-source software instead of paying for overpriced subscriptions. Niik-l's work turns Natron into a genuinely usable tool for VFX and matchmove work, and this fork adds the few things my own pipeline needed, in the same spirit: free, open, and installable by anyone. Everything here stays under Natron's GPL-2.0 licence, and any of these changes is welcome upstream.
>
> **What this fork adds** (branch [`diazfbx`](https://github.com/diazfbx/Natron/tree/diazfbx), one commit per feature so each can be picked separately):
>
> | Feature | What it does |
> |---|---|
> | **ScanlineRender — Wireframe Mesh + Opaque Wireframe** | New *Wireframe Mesh* choice (*Quads*, default / *Triangles*). In Wireframe shading, quads and n-gons from ReadGeo / ReadAlembicArchive are drawn with their own edges instead of the triangulation diagonals, like in Maya or Nuke. Edges are built only in Wireframe mode, so Shaded / Flat renders cost nothing extra. Procedural primitives (Card3D, Cube3D, …) still draw triangle edges. **Opaque Wireframe** hides the edges behind other surfaces (hidden-line wireframe, surfaces stay transparent over the plate); **Wireframe Color**, **Wireframe Width** (pixels) and **Wireframe Opacity** set the line look. These options only show when *Shading Mode* is *Wireframe*. |
> | **LD_3DE4 node + 3DE Lens Import** (`3DE` menu) | *LD_3DE4* (menu *Transform → 3DE4*, one entry per model named like the Nuke node, e.g. `LD_3DE4_Radial_Standard_Degree_4`, plus a generic `LD_3DE4` with a Model menu) runs every 3DEqualizer lens model of Science-D-Visions' LDPK 2.12 — the 18 models of the Nuke `LD_3DE4_*` / `LD_3DE_Classic_LD_Model` / `LD_OpenCV_*` nodes (radial, fisheye ×5, anamorphic standard / rescaled / stabilizer / homomorphic, Brown-Conrady, OpenCV, Classic) — with the Nuke knob names, Undistort / Distort and an STMap output. *3DE → Import Lens Distortion 3DE4 (.nk)* reads a 3DE `.nk` export and creates these nodes with the same model, direction and values, animated curves included, wired under the selected node (PyPlug `Plugins/PyPlugs/3DE_Lens_Import`; on a Natron without LD_3DE4 it falls back to `LensDistortion` for the 5 models that node has). |
> | **R3D import** (*File → Import R3D (REDline)…*, or a Read given a `.R3D`) | Converts a RED clip to an OpenEXR sequence with RED's free REDline tool (install REDCINE-X PRO; nothing from the RED SDK is used or shipped) and creates the Read (or switches the Read to the EXR) when it is done: decode resolution, ACES / REDWideGamut linear or Log3G10 / Rec.709, PIZ/ZIP compression, frame range, renumbering from 1001, RMD look. Runs in the background (REDline's window shows the progress) or waits; asks for REDline's location or offers its download when it is missing. |
> | **Cryptomatte node** (group *Keyer*) | A Nuke-style Cryptomatte keyer: *Layer* menu (CryptoObject / CryptoMaterial / CryptoAsset…), manifest read from the EXR header (or the `manif_file` sidecar), *Matte List* in Nuke's syntax, one name per line (quoted names, `*` / `?` wildcards, `<float>` IDs), viewer picker *Add* / *Remove*, *Preview Mattes*, *Matte Only*, *Invert*, *Matte Output* channel (alpha by default, or red / green / blue) with *Unpremultiply*. Panel laid out like Nuke's. Implements the open [Cryptomatte specification](https://github.com/Psyop/Cryptomatte) by Psyop. |
> | **Shuffle2: Wire / Classic** | *Shuffle2* (menu *Channel*) gets a *Mode* switch: *Wire* connects the input channels (B and A) to the output channels with cables like Nuke's Shuffle, *Classic* gives one drop-down per output channel. Both modes edit the same routing. The classic *Shuffle* node is unchanged, next to it. |
> | **Reformat in Nuke's layout** | *Center / Flip / Flop / Turn* on the *Resize Type* line, *Preserve BBox* next to *Black outside* (patch of openfx-misc, applied by `tools/win-build`). |
> | **Postage Stamp** (Node tab) | Every node can show a small image of its output on its box in the node graph, like Nuke's postage stamps (the former *Preview* checkbox, renamed, saved with the project and kept up to date even with the project's auto-previews off). |
> | **Viewer playback bar** | Light icons instead of black ones; with the Nuketron theme the bar is flat and translucent (borderless buttons, hover / checked highlights, orange current frame). |
> | **Check boxes before their label** | Like Nuke: `[x] Center`, `[x] Black outside`… everywhere in the properties panels. |
> | **Nuketron theme** | A Nuke-like look: Nuke greys, orange selection / active tabs / menus, yellow section titles, flat grey timeline with an orange playhead. Themes live in `<Natron>/Themes/` (one `.qss` per folder); pick one in *Preferences → Appearance → Stylesheet file*. |
> | **Modern ACES configs bundled** | The official OpenColorIO ACES configs are shipped in `Resources/OpenColorIO-Configs/`: `aces_2.1_cg`, `aces_2.1_studio`, `aces_1.3_cg`, `aces_1.3_studio` (copied verbatim from OpenColorIO's built-in configs, BSD-3-Clause). Pick one in *Preferences → Color Management*; the viewer's OCIO display/view menu then offers the ACES output transforms. |
> | **Windows build on MSYS2 UCRT64** | MSYS2 deprecated the MINGW64 environment and has been removing its packages (PySide6, OpenImageIO, OpenColorIO…) since October 2026, so `tools/win-build` failed with "target not found". The scripts and CMake files now follow the MSYS2 environment (UCRT64), install `clang` for shiboken6, and stop with a clear message in the wrong terminal. |
>
> **Build it** (Windows, in the *MSYS2 UCRT64* terminal, ~15 GB free on the build drive):
>
> ```bash
> git clone --branch diazfbx https://github.com/diazfbx/Natron.git
> cd Natron/tools/win-build
> bash build-all.sh
> ```
>
> The result is a self-contained `Natron-install/` folder (run `bin/Natron.exe` — same layout as the release archives: `bin/`, `lib/`, `Plugins/`, `Resources/`), which can be zipped and shared. See [tools/win-build/README.md](tools/win-build/README.md) and [BUILDING.md](BUILDING.md).
>
> **Vibe coded.** Like Niik-l's work underneath it, everything this fork adds was vibe coded: I'm a VFX artist, not a developer, and the code was written with an AI assistant (Claude) from my descriptions, screenshots and test scenes, then tested by me in production use. Expect rough edges and report them.
>
> **Status.** These additions are recent: the Cryptomatte node and the UCRT64 build changes have not been through a full release cycle yet. Please report problems in this fork's issues, and anything that also affects Niik-l's build in [Niik-l's issues](https://github.com/Niik-l/Natron/issues).

---

[![GPL2 License](http://img.shields.io/:license-gpl2-blue.svg?)](LICENSE.txt)

A fork of [Natron](https://github.com/NatronGitHub/Natron), the open-source node-based compositor, that adds a 3D system, the Cycles renderer + Cycles deep, a deep-compositing pipeline, a particle system, volumes, and an early 3D camera tracker — **69 new built-in nodes** — on a Qt6 / Python 3.14 / OpenGL 4.3 base. Windows and Linux builds.

- **Download:** [Releases](https://github.com/Niik-l/Natron/releases) — Windows portable archive (numbered releases), Linux AppImage attached to the same numbered releases; no installer either way
- **Node list:** [wiki](https://github.com/Niik-l/Natron/wiki) · developer notes in [NODE_REGISTRY.md](NODE_REGISTRY.md)
- **3D viewport navigation:** Maya/Nuke bindings by default; Blender and legacy presets, colours and startup defaults under Preferences → 3D Viewport ([wiki page](https://github.com/Niik-l/Natron/wiki/3D-Viewport-Preferences))
- **Bugs / ideas:** [Issues](https://github.com/Niik-l/Natron/issues) · **Chat / screenshare help:** [Discord](https://discord.gg/vZQNeJHqKB) (confirmed bugs still go to Issues so they stay tracked with the fix)
- **Build it yourself:** [BUILDING.md](BUILDING.md) · change history in [DEV_3D_SYSTEM_CHANGELOG.md](DEV_3D_SYSTEM_CHANGELOG.md)

## A disclaimer

This project is 100% vibe coded. I don't have a background in computer science or programming, and I don't consider myself a TD. I'm an artist who wanted a free way to clean up and grade HDRIs, and it grew from there. Expect rough edges, and please report them.

If you've found this fork useful, consider buying me a beer :) **[Ko-fi](https://ko-fi.com/niikl)** · **[PayPal](https://paypal.me/niikl)**

## What the fork adds

| Area | Nodes | Highlights |
|---|---|---|
| 3D scene | 9 geometry + 9 scene/render + 4 camera/light + 3 material | 3D viewport with gizmos, Card / Sphere / Cube / Cylinder, ReadGeo (glTF / GLB, FBX, OBJ, Alembic, as one mesh or part by part with file materials, morph targets and skinning), Alembic cameras, **GeoBuilder** (draw grids over the plate, projected through the camera), Project3D / UVProject / MergeMat, PBR Material3D with texture maps, Megascans loader |
| Cycles | in the scene/render set | Blender's Cycles path tracer as a node: materials, lights with **light-group AOVs**, HDRI domes, motion blur, DOF, AOV passes, shadow catcher and holdout, **CyclesRenderPass** that renders passes to versioned multi-layer EXR (deep EXR too) and builds the comp tree |
| ScanlineRender | in the scene/render set | GLSL scanline renderer with AOVs and particle modes, for fast previews |
| Deep compositing | 17 (+19 unreleased) | DeepRead / Write / Merge / Recolor / Flatten / Reformat / Expression …, deep to point cloud and back (**Blast** editing), Cycles native deep output |
| Particles | 18 | Emitter, solver, forces, spawn, collisions, ParticleMaterial, instancing on geo, rendered in ScanlineRender and Cycles |
| Volumes | in the geometry/render set | ReadVDB (fire / smoke), procedural Volume3D, **FastVolumeRender** real-time GPU preview |
| Matchmove | 2 | CameraTracker (detect / track / solve) and PointCloudGenerator |
| Depth of field | 2 | **LensDefocus** (physically based DOF: camera-driven blur, real lens designs ray traced through their glass, tilt-shift, GPU option) and DeepDefocus on deep images; Cycles can shoot through the same lenses |
| Colour + transform | 5 | GPU OCIO ACES views in the viewer (tested on the ACES 2.0 reference config), ColorChartMatch, SphericalTransform (8 projections), LensWarp |

Templates menu: ready-made graphs (this fork adds *Track*: Create Undistort, Create Distmap, Create Undistmap, Undisto Check — blank scenes stored as `.ntp` in `Resources/Templates/Track`, pasted into the current graph) for a basic 3D scene, the Megascans loader, Fast Volume Render, particle presets, and an HDRI face-edit rig.

**Underlying upgrades:** Qt5 → Qt6 + PySide6, Python 3.14, OpenGL 2.0 → 4.3 (compatibility profile), Eigen 3.4, C++17.

## Requirements

- Windows 10 or 11, 64-bit; or 64-bit Linux with glibc 2.28 or newer (any distribution from 2018 on — verified on Rocky 8/9, AlmaLinux 9, Fedora, Ubuntu 22.04/24.04, Debian 12, Arch and openSUSE Leap 15.6). The OpenGL 4.3 requirement rules out macOS.
- A GPU with OpenGL 4.3 for the 3D viewport, ScanlineRender and FastVolumeRender; FastVolumeRender additionally needs a Vulkan driver.
- Cycles renders on the CPU in the shipped builds.

## Installing

**Windows.** Download the `.7z` from the [Releases page](https://github.com/Niik-l/Natron/releases), extract it anywhere, and run `bin/Natron.exe`. The archive contains Natron, the bundled OpenFX plugins (openfx-io, openfx-misc providing both `Misc.ofx` and `CImg.ofx`, and openfx-arena for SVG/PSD/PDF/Krita import, Text and ImageMagick filters) and `NatronRenderer.exe` for command-line rendering. Nothing is written outside the folder except Natron's own settings.

**Linux.** Download the AppImage from the same [Releases page](https://github.com/Niik-l/Natron/releases) (each numbered release carries both platforms, built from the same commit), make it executable and run it:

```sh
chmod +x Natron-2.6-linux-x86_64.AppImage
./Natron-2.6-linux-x86_64.AppImage
```

It bundles the same things as the Windows archive: Qt, Python, OpenImageIO/OpenColorIO, OpenVDB, Cycles, FFmpeg (with x264/x265) and the openfx-io, openfx-misc and openfx-arena plugins. It needs only what a desktop provides (X11, Mesa or a vendor GL driver, fontconfig). On a Wayland desktop it runs through XWayland (AppImages before 2026-10-19 aborted at startup on a Wayland session with "Wayland contexts are not supported"; launch those with `NATRON_DISABLE_WAYLAND=1`). Inside a container without FUSE, add `--appimage-extract-and-run`. `NatronRenderer` for command-line rendering is inside the AppImage: `./Natron-*.AppImage --appimage-extract` and use `squashfs-root/usr/bin/NatronRenderer`. Each release names the commit it was built from; a matching debug-symbols tarball is attached for crash backtraces.

Verified on Rocky 8/9, AlmaLinux 9, Fedora, Ubuntu 22.04/24.04, Debian 12, Arch and openSUSE Leap 15.6 (CI, software GL) and on an NVIDIA RTX 4090 (GUI, 3D viewport, FastVolumeRender on Vulkan, Cycles). The 2026.10.02 AppImage crashed at startup on NVIDIA desktops; use 2026.10.06 or later. A GNOME Wayland session (Ubuntu 26.04, via XWayland) works from the 2026-10-19 fix on. Not yet tried: AMD or Intel GPUs. NixOS needs its AppImage wrapper; Alpine (musl) is not supported.

**Optional, but recommended:**

- **An ACES OCIO config.** The archive bundles Natron's classic configs (nuke-default, blender, natron, spi, aces 0.x). For a modern ACES pipeline — the one this fork is tested on — download the ACES 2.0 studio config from the [OpenColorIO-Config-ACES releases](https://github.com/AcademySoftwareFoundation/OpenColorIO-Config-ACES/releases) and point Natron at it: Preferences → Color → OpenColorIO config → Custom, then choose the `.ocio` file. (If an `OCIO` environment variable is set on your machine, Natron offers it as the "Environment variable (OCIO)" entry and uses it by default; picking any other entry overrides the variable for Natron only, so other apps such as Blender keep theirs.)
- **OpenRV** for the *Open in RV* buttons on Write and CyclesRenderPass. Get a build from the [OpenRV releases](https://github.com/AcademySoftwareFoundation/OpenRV/releases), then either set the `NATRON_RV_PATH` environment variable to the executable (`rv.exe` on Windows, `rv` on Linux) or fill in the RV Executable knob on the node. On Linux the OpenRV project publishes no binaries: build it from source (Rocky 8/9 are the supported targets) or use a distribution package, then point `NATRON_RV_PATH` at the resulting `rv`. The buttons themselves are platform-neutral.

To build from source, see [BUILDING.md](BUILDING.md) (Windows: MSYS2 / MinGW with an automated script set under `tools/win-build/`; Linux: the GitHub Actions workflows, documented in the same file).

## Contributors

- [alejandroua](https://github.com/alejandroua) — the universal ReadGeo: glTF / GLB and FBX import (cgltf, ufbx), Output Single Mesh and part-by-part modes with file materials, the Archive Contents tree, Re-build Materials, mesh-file sequences, morph targets and skinning, and a run of Cycles mesh-sync fixes (2026-10).

## Contributing

Bugs and feature requests go to this fork's [issue tracker](https://github.com/Niik-l/Natron/issues) — the forms ask for the build version, the area involved and your OCIO config, which is usually what's needed to reproduce a report. Bugs that also happen in official Natron belong [upstream](https://github.com/NatronGitHub/Natron/issues).

Development happens on `RB-2.6`; see [GIT_WORKFLOW.md](GIT_WORKFLOW.md) for the branch and commit conventions. Pull requests are welcome against `RB-2.6`.

There's a `.git-hooks` directory in the root with a `pre-commit` hook that checks code style (`astyle`):

```shell
cd Natron
mkdir .git/hooks
ln -s ../../.git-hooks/pre-commit .git/hooks/pre-commit
```

---

## About Natron

Natron is a free, open-source (GPLv2) video compositor, similar in functionality to Adobe After Effects, Foundry's Nuke, or Blackmagic Fusion. This fork is based on the NatronGitHub project; everything below is theirs.

- Website: https://natrongithub.github.io
- Source code: https://github.com/NatronGitHub/Natron
- Forum: https://discuss.pixls.us/c/software/natron
- Discord: https://discord.gg/cpMj5p3Fv5
- User documentation: https://natron.readthedocs.io/

### Features

- 32-bit floating-point linear color processing pipeline.
- Color management handled by [OpenColorIO](https://opencolorio.org/).
- Dozens of video and image formats supported such as: H264, DNxHR, EXR, DPX, TIFF, JPG, PNG through [OpenImageIO](https://github.com/OpenImageIO/oiio) and [FFmpeg](https://ffmpeg.org/).
- Support for many free, open-source, and commercial OpenFX plugins—currently almost all features of OpenFX v1.4 are supported.
  - [OpenFX-IO](https://github.com/NatronGitHub/openfx-io), [OpenFX-Misc](https://github.com/NatronGitHub/openfx-misc) and [OpenFX-Arena](https://github.com/NatronGitHub/openfx-arena) (ReadSVG, ReadPSD, ReadPDF, ReadKrita, ReadCDR, Text, ImageMagick filters) are bundled in this fork's releases on both platforms, plus [nik-kit-natron](https://github.com/Niik-l/nik-kit-natron), the fork's testing ground for new tools (a separate repo so this one stays uncluttered), under the **Nik Kit** menu; upstream Natron also ships [OpenFX-G'MIC](https://github.com/NatronGitHub/openfx-gmic), which this fork does not build
  - [All OFX products from RevisionFX](http://www.revisionfx.com), [Boris FX](https://borisfx.com/) OpenFX plugins including Sapphire, [Furnace by The Foundry](http://www.thefoundry.co.uk/products/furnace/), and many more.
- Intuitive user interface: Natron aims not to break habits by providing an intuitive and familiar user interface. It is possible to customize and separate the graphical user interface on any number of screens. You can re-use your layouts and share your layout files (.nl).
- Performance: In Natron, anything you do produces real-time feedback in the viewer thanks to the optimized multi-threaded rendering pipeline and support for proxy rendering (computing at a lower resolution to speed up rendering).
- Multi-task: Natron can render multiple graphs at the same time. It can also be used as a background process in headless mode.
- Recover easily from bugs: Natron's auto-save system detects inactivity and saves your work for yourself. Natron is also able to render frames in a separate process, meaning that any crash in the main application would not crash the ongoing render (and the other way around).
- Project files saved in XML and easily editable by humans.
- Fast & interactive viewer - Smooth & accurate zooming/panning even for very large image sizes (tested on 27k x 30k images).
- Real-time playback: Natron offers real-time playback with excellent performance thanks to its RAM/Disk cache. Once a frame is rendered it can be reproduced instantly afterward, even for large image sizes.
- Animate your visual effects: Natron offers a simple and efficient way to deal with keyframes with a very accurate and intuitive Curve Editor as well as a Dope Sheet to quickly edit your motion graphics.
- Command-line rendering: Natron is capable of running without a GUI for batch rendering with scripts or on a render farm.
- Rotoscoping, rotopainting, and tracking support
- Multi-view workflow: Natron saves time by keeping all the views in the same stream. You can separate the views at any time with the OneView node.
- Python scripting integration: parameter expressions, user-defined parameters, node groups as Python scripts, a script editor, Python callbacks on internal events, and PySide integration so the interface is extensible with new menus and windows.
- Multi-channel compositing: Natron can manipulate multi-layered EXR files thanks to OpenImageIO. Users can choose to work with any layer or channel on any node, new custom layers can also be created.
