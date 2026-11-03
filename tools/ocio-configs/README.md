# Modern ACES OpenColorIO configs

The OpenColorIO-Configs pack Natron downloads (`NatronGitHub/OpenColorIO-Configs`,
tag `Natron-v2.4`) only carries pre-1.0 ACES configs (`aces_0.1.1`, `aces_0.7.1`).
These four folders add the official ACES configs, copied verbatim from the
built-in configs of OpenColorIO (`src/OpenColorIO/builtinconfigs/configs/`,
commit 22dd9c4faa0d1b7cfb3d64d1a6e9cb454acc6cfd, BSD-3-Clause — see
`LICENSE-OpenColorIO.txt`):

| Folder (name in Preferences → Color Management) | Source config | Needs OCIO |
|---|---|---|
| `aces_2.1_cg`     | `cg-config-v5.0.0_aces-v2.1_ocio-v2.6`     | 2.6 |
| `aces_2.1_studio` | `studio-config-v5.0.0_aces-v2.1_ocio-v2.6` | 2.6 |
| `aces_1.3_cg`     | `cg-config-v2.2.0_aces-v1.3_ocio-v2.4`     | 2.4 |
| `aces_1.3_studio` | `studio-config-v2.2.0_aces-v1.3_ocio-v2.4` | 2.4 |

- **CG** configs: the essentials for CG / comp work — ACEScg (the `scene_linear`
  role), ACES2065-1, ACEScct, Linear Rec.709, sRGB, Rec.709, Display P3, …
- **Studio** configs: everything in CG plus camera log spaces (ARRI LogC3/LogC4,
  RED Log3G10, Sony S-Log3, Canon Log, Panasonic V-Log, …) and more displays.

They are self-contained (built-in transforms only, no LUT files). Each
`config.ocio` is a plain copy; to update, copy the newer files from the same
OpenColorIO directory and adjust the table above.

`tools/win-build/06-install.sh` copies every folder here into
`Natron-install/Resources/OpenColorIO-Configs/`, next to the classic pack.
For a dev build, copy them into `build-qt6/Resources/OpenColorIO-Configs/`.
