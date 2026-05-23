# Polyphase PS2 Build Target

A Polyphase Engine native addon that adds **Sony PlayStation 2** as a build
target. Selecting `Sony PS2 (PS2SDK + gsKit)` from the Build & Run menu cooks
the project, cross-compiles via PS2SDK (`mips64r5900el-ps2-elf-gcc`), produces
a bare ELF, and optionally wraps it into an `mkisofs` `.iso` for PCSX2 or
FreeMcBoot. The editor-side binary never links against PS2SDK — every
PS2-specific symbol lives in the addon's `Runtime/PS2/` tree and is compiled
into the user's PS2 ELF, not into the editor.

## What this addon provides

- **Build pipeline integration** (Variant 2 platform-extension): generated
  `PolyphasePlatform_*.h` bridge headers route engine type forks
  (`ThreadObject`, `MutexObject`, `SocketHandle`, etc.) to PS2-specific
  definitions without modifying engine source.
- **GS rendering backend** (`Runtime/PS2/Graphics_PS2GS/`) — implements the
  engine's `GFX_*` surface on top of gsKit + dmaKit:
  - NTSC 640×448 framebuffer, 24-bit color + 16-bit Z, interlaced double-buffer
  - Textured static meshes, vertex-color meshes (TileMap2D / Terrain3D /
    Voxel3D), particle quads, UI widgets, font text
  - 3-light directional + ambient lighting via CPU MVP transform + per-vertex
    Gouraud (PS2 has no programmable shaders; lighting is done EE-side and
    passed as per-vertex color to the GIF)
- **System runtime** (`Runtime/PS2/System_PS2.cpp`) — file I/O, threads,
  semaphores, timers via PS2SDK kernel; **memory-card save data** via libmc
  (mcman/mcserv IRX), with icon.sys + browser-visible save folders.
- **Audio mixer** (`Runtime/PS2/Audio_PS2.cpp`) — software 8-voice mixer +
  parallel streams, feeding a single audsrv output channel. Real audsrv-based
  pacing via `audsrv_wait_audio`. See [Documentation/Audio.md](Documentation/Audio.md).
- **DualShock 2 input** (`Runtime/PS2/Input_PS2.cpp`) — libpad-based, both
  thumbsticks + pressure-sensitive L2/R2 → analog axes.
- **`Makefile_PS2`** — wildcard-discovers engine + addon + project sources;
  resolves PS2DEV from env/common paths automatically; embeds `audsrv.irx`
  into the ELF via `bin2c` so the IOP can load it from EE RAM (works under
  PCSX2 `-elf` and from disc alike — no separate IRX file shipped).

## Hardware target

The PS2 is a fixed-function console with the following hard limits. Code that
exceeds these will fail to render, crash, or silently produce wrong output.

### CPU — Emotion Engine (EE)

| Spec | Value |
| --- | --- |
| Core | R5900 (custom MIPS III superscalar, 32-bit address space, 128-bit GPR for SIMD) |
| Clock | 294.912 MHz |
| Bus clock | 147.456 MHz (used for timer→µs conversion in `SYS_GetTime`) |
| L1 cache | 16 KB I + 8 KB D |
| Scratchpad RAM (SPR) | 16 KB on-chip, 1-cycle access — not used by this addon |
| FPU | 32-bit single-precision, **not IEC559-conformant** — no NaN, no denormals, non-standard rounding. `std::numeric_limits<float>::is_iec559 == false`. |
| VU0 / VU1 | Two vector co-processors, 128-bit SIMD. Used by gsKit's GIF packet pipeline; not directly invoked by addon code. |
| Toolchain | `mips64r5900el-ps2-elf-gcc` (PS2SDK 2024+) |
| `double` | Hardware single-precision only — `double` falls back to slow software emulation. Avoid in hot paths. |

> **GLM compatibility note**: because the EE FPU is not IEC559, GLM's
> `clamp`/`round`/etc. `static_assert(is_iec559 || is_integer)` rejects PS2
> `float`. The Makefile sets `-DGLM_FORCE_UNRESTRICTED_GENTYPE` which adds a
> permissive third branch to the assert. Same workaround used by every PS2
> homebrew that includes GLM.

### IOP (I/O Processor)

| Spec | Value |
| --- | --- |
| Core | MIPS R3000A, 36 MHz (boot) → 37.5 MHz (game-mode auto-bump) |
| Purpose | I/O, audio (audsrv → SPU2), memory card, USB, controllers, sound, optical-disc reads. The EE talks to IOP-side servers via SIF RPC. |
| IRX modules loaded | `SIO2MAN` + `PADMAN` (input), `LIBSD` + `audsrv` (audio), `MCMAN` + `MCSERV` (memory card) |

### GPU — Graphics Synthesizer (GS)

| Spec | Value |
| --- | --- |
| Clock | 147.456 MHz |
| Pipeline | **Fixed-function** — no programmable shaders. Lighting/blending/texture are all configured via GIF packets and GS register writes. |
| Fill rate | 1.2 GPixels/s (textured) — fillrate-bound for fullscreen alpha layers |
| VRAM | **4 MB** embedded, 48 GB/s bandwidth |
| Triangle rate | 75 M tri/s theoretical, realistic 2–5 M for textured + per-vertex color |
| Output | Interlaced NTSC 640×448 (this addon), PAL 640×512 (Phase 3+) |

### Memory

| Region | Size | Notes |
| --- | --- | --- |
| **Main RAM (EE)** | **32 MB** | Same budget as PSP — engine code + Lua + assets share this. Embedded assets are deliberately disabled in `Makefile_PS2`. |
| **VRAM (GS)** | 4 MB | Front + back buffer + 16-bit depth + texture cache. gsKit manages allocation. |
| **IOP RAM** | 2 MB | IRX modules + audsrv ring buffer + libmc state. Not directly addressable from EE. |
| **Memory Card** | 8 MB (Sony) | Saves at `mc0:/BXDATA-POLY0001/<saveName>`. |
| Scratchpad RAM | 16 KB (EE) | On-chip, 1-cycle. Unused by this addon. |

VRAM allocation (gsKit-managed):

```
0x000000 ─┐
          │  Front framebuffer  640 × 448 × 4 = 0x118000 bytes (1120 KB)
0x118000 ─┤
          │  Back framebuffer   640 × 448 × 4 = 0x118000 bytes
0x230000 ─┤
          │  16-bit depth       640 × 448 × 2 = 0x8C000 bytes (560 KB)
0x2BC000 ─┘  (about 1.3 MB remaining for VRAM textures, managed by gsKit_TexManager)
0x400000     (end of VRAM)
```

### Display

| Spec | Value |
| --- | --- |
| Native (this addon) | 640 × 448 NTSC interlaced |
| PAL (planned) | 640 × 512 |
| Aspect ratio | 4:3 (anamorphic 16:9 selectable in BIOS, not currently exposed) |
| Refresh | 59.94 Hz (NTSC) / 50 Hz (PAL) |
| Color format | `GS_PSM_CT24` (24-bit RGB) |
| Depth buffer | `GS_PSMZ_16S` (16-bit signed) |

### Texture limits

| Spec | Value |
| --- | --- |
| Max texture size | **1024 × 1024 per mip level** (GS HW limit, log2 0..10) |
| Width / height | **Must be power of 2** (`gsKit_TexManager_bind` enforces). Non-PoT is not addressable. |
| Pitch alignment | Power-of-2 widths are inherently aligned; gsKit handles GS Buffer-Width register internally. |
| Formats wired | RGBA8888 (`GS_PSM_CT32`), RGB888 (`GS_PSM_CT24`), CLUT8 (`GS_PSM_T8`) via gsKit's loader |
| Filtering | `GS_FILTER_LINEAR` / `GS_FILTER_NEAREST` — addon uses linear |
| Mipmaps | Not used (GS supports up to 7 mip levels) |
| Wrap | `GS_CMODE_REPEAT` default; `GS_CMODE_CLAMP` available per-texture |

### Audio

| Spec | Value |
| --- | --- |
| Backend | **audsrv** IRX (canonical PS2 homebrew audio) feeding SPU2 via LIBSD |
| Sample rate (this addon) | 44.1 kHz, 16-bit signed stereo PCM |
| Voice count | 8 software voices (engine `AUDIO_MAX_VOICES`) + 4 stream slots, mixed on EE |
| Pacing | `audsrv_wait_audio(N)` blocking primitive — see [Documentation/Audio.md](Documentation/Audio.md) |

### Input

| Spec | Value |
| --- | --- |
| Backend | libpad (`SIO2MAN` + `PADMAN` IRX) |
| Devices | DualShock 2 in port 0 slot 0 |
| Buttons | × ○ □ △ L1 L2 R1 R2 Start Select D-pad L3 R3 |
| Sticks | 2 analog thumbsticks |
| Pressure | DS2 face-button and L2/R2 pressure read as analog axes 0..255 |
| Vibration | Wired via `padSetActAlign` (small + large motor) |

### Network

| Spec | Value |
| --- | --- |
| Backend | Stub (`Network_PS2.cpp` returns "no network") |
| Real hardware | Network Adapter (NEEDS `ps2ip` + `ps2smap` IRX, deferred) |

## Current support status

| Subsystem | State | Notes |
| --- | --- | --- |
| Engine boot + main loop | OK | `Main_PS2.cpp` — SIF init, IOP reset, sbv patches, IRX stack |
| File I/O | OK | newlib stdio over host:/cdrom0:/mc0:; directory iteration stub |
| Threads, semaphores, timers | OK | PS2SDK kernel — `CreateThread`/`CreateSema`/`GetSystemTime` |
| Logging | OK | `scr_printf` boot tty + `polyphase.log` on host: or mc0: |
| **Textured static meshes** | OK | gsKit `gsKit_prim_triangle_goraud_texture_3d`, CPU MVP + per-vertex Gouraud |
| **Vertex-color meshes** (TileMap2D / Terrain3D / Voxel3D) | OK | Shared `DrawVertexColorMesh` helper, unlit + alpha blend |
| **Particles** | OK | 4→6 vertex expansion per particle, billboard quads |
| **UI widgets** (Quad / Text / Poly) | OK | gsKit-rendered, font glyph atlas |
| **Lighting** | OK | 3 directional + ambient, CPU-shaded into per-vertex color |
| **Audio mixer + streams** | OK | Phase 3 — see [Audio.md](Documentation/Audio.md) |
| **DualShock 2 input** | OK | libpad both sticks + pressure analog |
| **Memory card saves** | OK | libmc with icon.sys writer — see [MemoryCard.md](Documentation/MemoryCard.md) |
| Skeletal meshes (skinning) | Stub | Phase 4 — CPU skinning + repack, no HW bones |
| Shadow mapping | None | GS can't sample depth as texture |
| Post-processing | None | GS has no spare fillrate; readable framebuffer requires VRAM blit |
| Light bake / path trace | None | Vulkan-only by design |
| Network | Stub | Returns "no network"; ps2ip/ps2smap IRX work deferred |

## Build environment

| Requirement | Notes |
| --- | --- |
| PS2DEV | https://github.com/ps2dev/ps2dev/releases — install to `C:\ps2dev` (Windows), `/usr/local/ps2dev` (Linux/macOS), or anywhere and set `PS2DEV=...`. |
| make | GNU make ≥ 4.0. The Windows ps2dev bundle ships its own; otherwise install via MSYS2 or Git Bash. |
| WSL (Windows, **opt-in**) | Available as a fallback via the **Use WSL** Target Option. The addon auto-translates `C:\...` to `/mnt/c/...`. |
| PCSX2 | https://pcsx2.net — for emulator launches. `pcsx2-qt.exe` is the canonical binary on Windows 1.7+. |
| mkisofs | Optional, only when `ps2.makeIso=1`. Supplied by cdrtools (Linux/macOS) or MSYS2 (`pacman -S mkisofs`) on Windows. |

The Makefile resolves PS2DEV in this order: command-line `PS2DEV=...` → env
var → common install paths (`/usr/local/ps2dev`, `/opt/ps2dev`, `$HOME/ps2dev`,
`/mnt/c/ps2dev`, `/mnt/d/ps2dev`). It validates by probing for
`$(PS2DEV)/ps2sdk/samples/Makefile.eeglobal_cpp` and aborts with an
actionable message if the sentinel is missing.

## Profile options (per Build Profile)

The Build Profile UI for the PS2 target exposes:

- **Title** — shown in PCSX2 and on real PS2 BIOS browsers
- **Disc ID** — 11-char `SYSTEM.CNF` style (default `POLY00001`)
- **Make ISO** — wrap the ELF + boot files into a mkisofs ISO image
- **Custom Makefile** — override `Makefile_PS2` path if you have a fork
- **Parallel Jobs** — `make -j<N>` (default **4**). See "Host RAM and parallel jobs" below.
- **Use WSL** (Windows only) — opt-in fallback when native ps2dev isn't installed
- **PS2DEV path** — overrides auto-detect
- **PCSX2 path** — overrides `PS2_EMULATOR` env var

## Host RAM and parallel jobs

Engine translation units compile heavily under `ee-gcc`. Engine.cpp,
Renderer.cpp, Bullet, Vorbis, and the GS draw helpers each peak around
**1–1.5 GB of RAM** during their compile. Naive `make -j` (no limit) spawns
one job per CPU core, so on a 12-core box you can commit 18 GB before the
OS, IDE, or shell get a turn — enough to freeze a 16 GB host into swap death.

The addon defaults `-j4`. Rule of thumb:

| Host RAM | Safe jobs | Notes |
| --- | --- | --- |
| 8 GB | 2 | Tight. Close other apps before building. |
| 16 GB | **4** (default) | Headroom for IDE / browser. |
| 32 GB | 8–12 | Most workstations. |
| 64 GB | 16+ | Beefy desktop / CI runner. |

Tune via the **Parallel Jobs** slider in the Build Profile. The value is
clamped to `[1, 64]` server-side and silently falls back to `4` if you somehow
inject garbage.

## Known quirks (from Phase 0–3 development)

- **GLM's IEC559 assert** — fixed via `-DGLM_FORCE_UNRESTRICTED_GENTYPE`. Do
  not remove this from the Makefile; the EE FPU genuinely isn't conformant.
- **`-G0` is owned by `Makefile.eeglobal_cpp`** — do NOT add `-G0` to
  `EE_CFLAGS` in `Makefile_PS2`; overriding it breaks the small-data area.
- **Mixer thread `$gp` register** — the audsrv mixer thread captures `$gp`
  at thread-create time via inline asm (`asm volatile("move %0, $gp"
  : "=r"(currentGp))`). Without this, the thread's global-data accesses
  trap. Standard PS2 idiom.
- **`audsrv_play_audio` is non-blocking** — `audsrv_wait_audio(N)` is the
  blocking primitive. Don't pace the mixer with `usleep` (we tried; it
  caused single-buffer playback then silence — see
  [Documentation/Audio.md](Documentation/Audio.md)).
- **Forward references in compilation unit** — PS2 builds are
  single-translation-unit-per-file (no LTO across .o), so any helper a
  function uses must be declared above the function. `Graphics_PS2GS.cpp`
  and `System_PS2.cpp` both had recurring forward-reference build breaks
  while phases were being assembled.
- **Embedded asset blob is OFF** — `Makefile_PS2` deliberately excludes
  `EmbeddedAssets.cpp`. The EE has only 32 MB main RAM and a cooked-asset
  blob would consume more than half of it. Assets load from host:/cdrom0:/
  mc0:/ at runtime via `AssetManager::Discover`.

## Files of interest

| Path | Purpose |
| --- | --- |
| `Source/ComPolyphaseBuildTargetPs2.cpp` | Editor-side addon DLL: build target descriptor + `GetCompileCommand` + `PostPackage` + `RunInEmulator` callbacks |
| `Makefile_PS2` | PS2SDK build wrapper — invoked by the addon's `GetCompileCommand`. Override with the **Custom Makefile** profile option to point at a fork. |
| `Runtime/PS2/Main_PS2.cpp` | EE entry point — SIF init, IOP reset, sbv patches, IRX stack loader (SIO2MAN, PADMAN, LIBSD, audsrv, MCMAN, MCSERV) |
| `Runtime/PS2/System_PS2.cpp` | `SYS_*` implementations — file I/O, threads, timers, libmc save data |
| `Runtime/PS2/Audio_PS2.cpp` | audsrv mixer thread + voice/stream API |
| `Runtime/PS2/Input_PS2.cpp` | libpad DualShock 2 input |
| `Runtime/PS2/Graphics_PS2GS/Graphics_PS2GS.cpp` | gsKit-based `GFX_*` implementation |
| `Runtime/PS2/Graphics_PS2GS/PS2GSTypes.h` | Vertex struct layouts + GS register constants |
| `Runtime/PS2/Graphics_PS2GS/PS2GSUtils.{h,cpp}` | glm ↔ PS2 type conversion |
| `Runtime/PS2/SystemTypes_Platform.h` | PS2 typedefs surfaced through Variant 2 bridge |
| `Runtime/PS2/InputTypes_Platform.h` | PS2 input types |
| `Runtime/PS2/AudioTypes_Platform.h` | Empty — internal to `Audio_PS2.cpp` |
| `Runtime/PS2/NetworkTypes_Platform.h` | `SocketHandle = int32_t` (stub) |

## Documentation

- [Architecture](Documentation/Architecture.md) — EE / IOP / GS / SPU2 division of work, IRX loading, threading model
- [Audio](Documentation/Audio.md) — audsrv mixer, blocking primitive choice, voice + stream model
- [Graphics](Documentation/Graphics.md) — gsKit, GIF packets, VRAM layout, CPU MVP + per-vertex shading
- [Memory Card](Documentation/MemoryCard.md) — libmc async API, icon.sys, save folder layout
- [Build](Documentation/Build.md) — Makefile_PS2 internals, PS2DEV setup, audsrv.irx embedding

## References

- [ps2dev/ps2dev](https://github.com/ps2dev/ps2dev) — toolchain releases
- [ps2dev/ps2sdk](https://github.com/ps2dev/ps2sdk) — SDK source
- [gsKit](https://github.com/ps2dev/gsKit) — GS abstraction library
- [PCSX2](https://pcsx2.net) — emulator
- [audsrv source](https://github.com/ps2dev/ps2sdk/tree/master/iop/sound/audsrv) — IOP-side audio server
