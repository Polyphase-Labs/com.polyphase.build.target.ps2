# Third-Party Notices

This addon incorporates third-party material. Each item below is listed with its
license and the exact scope of what is included.

---

## XTC — VU1 microcode

**Upstream:** https://github.com/aap/xtc (commit `b0a01f5`)
**Copyright:** (c) 2024 aap
**License:** MIT — full text in `Runtime/PS2/Graphics_PS2GS/vu1/LICENSE.xtc`

**What is included:** the VU1 microcode only, vendored into
`Runtime/PS2/Graphics_PS2GS/vu1/`:

```
im3d.dsm            the nolight 3D microprogram
default_proc.vu     transform / perspective divide / fog / XGKICK
default_clipproc.vu clip-buffer output stage
cliptri.vu          6-plane homogeneous triangle clipping
clipline.vu         the same for line segments
TLclip.vu TSclip.vu triangle list / strip clipping drivers
LLclip.vu LSclip.vu line list / strip clipping drivers
PointCull.vu        point culling
defines.inc         VU memory map and UNPACK codes
joinvu              3-line preprocessing helper
```

Each file carries a header identifying its origin. The sources are unmodified
except for CRLF→LF normalisation and the addition of that header.

**What is NOT included:** XTC's GS layer (`xtcg.c`), texture layer
(`xtctex.c`), demo scaffolding (`main.c`, `scenes.c`, `joy.c`, `fio.c`), the
portable model/animation layer (`common/`), the OpenGL backend (`src_gl/`), the
generated sample data (`src/data/`), and the `xtc-assets` sample submodule.
XTC's vendored copy of **lodepng** (zlib license) is likewise not included,
because the texture layer that used it is not used here — gsKit's texture
manager is used instead.

**No Sony/SCE SDK material is included.** XTC's PS2 backend is written against
the Sony EE SDK, but this addon does not build against it. The microcode itself
contains no `SCE_*` references at all. The nine `SCE_VIF1_SET_*` macro uses that
appear elsewhere in XTC are not vendored; where equivalent functionality is
needed it comes from ps2sdk's own `packet2_vif.h` (`MAKE_VIF_CODE`).

---

## Pre-existing build dependencies

These are required to build the PS2 target but are not redistributed as part of
this addon; they are supplied by the developer's `ps2dev` installation.

| Component | Role |
|---|---|
| **ps2sdk** | EE toolchain headers/libraries, `packet2` DMA/VIF packet builder |
| **gsKit / dmaKit** | GS initialisation, VRAM and texture management, draw queue |
| **GNU binutils (`dvp` target)** | `dvp-as`, assembles the VU1 microcode |

---

## Note on this addon's own license

This addon does not currently declare a license: there is no `LICENSE` file and
no `license` field in `package.json`. That should be settled before the addon is
distributed. Whatever is chosen, the MIT notice for XTC above must ship with any
distribution that includes `Runtime/PS2/Graphics_PS2GS/vu1/`.
