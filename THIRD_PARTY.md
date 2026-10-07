# Third-party components

tf2mt's own code (scripts, launcher, patch tools) contains no code copied from the projects below.
The **tf2mt runtime** release asset (`tf2mt-runtime-*.tar.xz`) redistributes these components unmodified,
except where noted:

| Component | Version | License | Source |
|---|---|---|---|
| Wine (Sikarugir build, wine-staging based) | 10.0 (`WS12WineSikarugir10.0_6`) | LGPL-2.1-or-later | https://gitlab.winehq.org/wine/wine · https://github.com/Sikarugir-App |
| DXVK (async-capable build) | 2.4.1 | zlib | https://github.com/doitsujin/dxvk |
| MoltenVK | 1.4.1 | Apache-2.0 | https://github.com/KhronosGroup/MoltenVK |
| DXMT | 0.80 | see upstream | https://github.com/3Shain/dxmt |
| vkd3d | bundled with Wine | LGPL-2.1-or-later | https://gitlab.winehq.org/wine/vkd3d |

**Modification:** on the user's machine, `tools/wine-patches/winemac_warp_nodiscard.py` binary-patches two
instructions in Wine's `winemac.so` (see `docs/mouse-input.md`). The shipped runtime is unmodified; the patch is
applied locally and can be reverted. The corresponding Wine source is `dlls/winemac.drv/cocoa_app.m`,
`-[WineApplicationController setCursorPosition:]`, in Wine 10.0.

**Not included and never redistributed:** Steam, Team Fortress 2 and any Valve content (art, fonts, game files).
The launcher loads TF2 art and fonts at runtime from the user's own installation.
