# PS2 Architecture — Polyphase Build Target

This document explains how the PS2 build target maps Polyphase Engine
subsystems onto the PS2's split-processor architecture. Read this first if
you're new to PS2 programming or to this addon.

## The three processors

The PS2 is really three computers in one chassis, talking over a shared bus:

```
        ┌─────────────────────────┐
EE  ──> │ R5900 Emotion Engine    │  EE-side code: engine + game logic, GS
        │ 294 MHz, 32 MB RAM      │  draw packet building, audsrv RPC stubs
        │ VU0, VU1 (SIMD)         │
        └───────────┬─────────────┘
                    │ SIF RPC bus
        ┌───────────┴─────────────┐
IOP ──> │ R3000A I/O Processor    │  IRX modules — audsrv, mcman/mcserv,
        │ 37.5 MHz, 2 MB RAM      │  SIO2MAN, PADMAN, LIBSD, fileXio
        │ Owns DMA channels       │
        └───────────┬─────────────┘
                    │
        ┌───────────┴─────────────┐
GS  ──> │ Graphics Synthesizer    │  Fixed-function rasteriser. EE writes
        │ 147 MHz, 4 MB VRAM      │  GIF packets describing primitives;
        │ Fixed-function GPU      │  GS rasterises with no programmability.
        └─────────────────────────┘
```

The EE is "the CPU" you write code on. The IOP and GS are subordinate
processors that respond to commands. Audio (SPU2) and memory-card I/O live
on the IOP; rendering lives on the GS.

## EE — what runs here

Everything in `Runtime/PS2/*.cpp` (except IRX-side code, which isn't shipped
here) runs on the EE. That includes:

- Polyphase Engine code (Engine.cpp, Renderer.cpp, World.cpp, asset loaders)
- Lua VM + scripts
- Bullet physics, recastnavigation
- Vorbis decoder
- `Graphics_PS2GS.cpp` — CPU-side MVP transform, per-vertex shading,
  GIF-packet emit via gsKit
- `Audio_PS2.cpp` — software mixer that *produces* PCM, then RPCs to audsrv
- The Game thread (your project's `Source/*.cpp`)

The EE has two vector co-processors (VU0/VU1) for SIMD. The addon does NOT
hand-write VU microcode — gsKit handles VU usage internally for GIF packet
chaining. If you need raw VU performance, that's a Phase-4+ topic.

## IOP — what's loaded here

The IOP is dormant until the EE loads IRX modules into it. `Main_PS2.cpp`
loads them at boot in this order:

| Order | Module | Source | Purpose |
| --- | --- | --- | --- |
| 1 | `SIO2MAN` | `rom0:` | I/O bus driver — required by libpad and libmc |
| 2 | `PADMAN` | `rom0:` | Controller RPC server |
| 3 | `LIBSD` | `rom0:` | Low-level SPU2 driver (audsrv depends on it) |
| 4 | `audsrv` | EE RAM (`bin2c`-embedded) | Polyphase's audio mixer feeds this |
| 5 | `MCMAN` | `rom0:` | Memory-card hardware driver |
| 6 | `MCSERV` | `rom0:` | Memory-card filesystem |

After each load, the EE-side stub library (libpad / libaudsrv / libmc) is
initialised: `padInit` / `audsrv_init` / `mcInit`.

`audsrv.irx` is unique — it's not on `rom0:` (the BIOS ROM filesystem), so
it has to come from somewhere. The Makefile embeds it into the ELF via
`bin2c`, which converts the IRX bytes to a C source file with two extern
symbols:

```c
unsigned char audsrv_irx[] = { 0x7f, 0x45, 0x4c, 0x46, ... };
unsigned int  size_audsrv_irx = sizeof(audsrv_irx);
```

`Main_PS2.cpp` passes these to `SifExecModuleBuffer`, which DMAs them to the
IOP and starts the module. This works under PCSX2 `-elf` boot, from CD/DVD
disc, and from FreeMcBoot's USB/MC paths alike — no separate IRX file ships
alongside the ELF.

## GS — what's drawn here

The GS is a stupid-fast pixel pump with no programmability:

- Triangle rasterisation
- Per-vertex Gouraud color interpolation
- Texture sampling (bilinear / nearest)
- Alpha blending (configurable)
- Z-test + Z-write
- Scissor / dithering

It cannot do:

- Programmable shaders (no fragment/vertex shaders — period)
- Sampling its own depth buffer (so no shadow maps)
- Frame-buffer readback within a draw (so no post-processing without a
  separate VRAM blit)

The EE drives the GS by building **GIF packets** — small command headers
plus per-vertex data — and DMAing them via the GIF DMA channel. gsKit wraps
this; you call `gsKit_prim_triangle_goraud_texture_3d(...)` and gsKit emits
the right packet.

Lighting on this addon is done CPU-side on the EE: `World::GetMainLight` →
per-vertex N·L → bake into the vertex's RGBA → emit Gouraud triangle. The GS
just interpolates. See [Graphics.md](Graphics.md).

## SPU2 — audio hardware

The SPU2 lives inside the GS chip but is functionally part of the IOP audio
stack. It plays back PCM from an internal ring buffer that the IOP keeps
fed via the LIBSD driver. audsrv sits on top of LIBSD and exposes a
buffered-streaming API to the EE.

The Polyphase audio flow:

```
Engine  ─────> AUD_Play  ─┐
                          │
                   sVoices[] table on EE
                          │
                          ▼
                   MixOneBuffer() — software mixer, 1024 frames @ 44.1 kHz
                          │
                          ▼
                   audsrv_wait_audio(N)  (blocks until ring has room)
                          │
                          ▼
                   audsrv_play_audio(buf, N)  (DMAs to IOP)
                          │
                          ▼
                   audsrv IRX (IOP)
                          │
                          ▼
                   LIBSD → SPU2 → speaker
```

Why software-mix on the EE instead of using the SPU2's 24 hardware ADPCM
voices: parity with the PSP / Wii / 3DS paths in the engine, simpler resampling
(per-voice fractional cursor), unlimited voice count, and no SPU2-RAM
allocation pressure. See [Audio.md](Audio.md).

## Threading model

The EE has cooperative kernel threads. The addon creates:

- **Main thread** — runs `main()` → Polyphase's engine loop. Default
  priority `0x40`.
- **Audio mixer thread** — created by `AUD_Initialize`. Priority `0x30`
  (higher than main, lower numeric = higher priority on EE), 16 KB stack.
  Spends nearly all its time blocked in `audsrv_wait_audio`, so doesn't
  starve the game thread.

Synchronisation primitives in use:

- `CreateSema`/`WaitSema`/`SignalSema` — counting semaphore (initial 1, max
  1 gives mutex behaviour). Both `Audio_PS2` and `System_PS2` use this
  pattern.
- `CreateThread` requires a `gp_reg` value pointing at the calling thread's
  global pointer. The audio mixer captures this at create-time via inline
  asm:
  ```cpp
  void* currentGp = nullptr;
  asm volatile("move %0, $gp" : "=r"(currentGp));
  th.gp_reg = currentGp;
  ```
  Standard PS2 idiom. Without it, global accesses in the new thread trap.
- `ReferThreadStatus(thid, &info)` — the canonical PS2 EE pattern for
  "wait until thread exits" (the EE kernel doesn't expose a portable
  `WaitThread`). Used in `AUD_Shutdown` to drain the mixer before deleting
  its semaphore. Capped at 250 ms so a wedged audsrv can't deadlock
  shutdown.

## Boot sequence

`Main_PS2.cpp::main()` is the EE entry point. Order matters — most steps
have to run before any later step can succeed:

1. `SifInitRpc(0)` — wakes the EE↔IOP RPC bus. Required for every libloadfile
   / fileXio / smap / audsrv call. Cheap on PCSX2; ~1 ms on real hardware.
2. `SifIopReset` + `SifIopSync` loop — resets the IOP and waits for it to
   come back. Mandatory under PCSX2 `-elf` boot because the IOP may be in
   an unknown state; on real hardware booting from disc the BIOS does this
   for us, but calling it again is safe.
3. `SifInitRpc(0)` again — re-init after the IOP reset.
4. `sbv_patch_enable_lmb` + `sbv_patch_disable_prefix_check` — lift the
   "module must be on protected media" check so we can `SifLoadModuleBuffer`
   from EE RAM (needed for embedded `audsrv.irx`).
5. `SifLoadFileInit` + `SifInitIopHeap` — open the LoadFile RPC channel and
   set up the IOP-side allocator. Order matters here too: LoadFileInit
   first, then IopHeap.
6. `init_scr()` — `scr_printf` tty so the boot phase has on-screen logging
   before gsKit takes over.
7. Load IRX stack (see table above).
8. `GameMain(argc, argv)` — passes control to the engine. Polyphase calls
   `AUD_Initialize`, `GFX_Initialize`, etc., from there. The `OctPre*` /
   `OctPost*` hooks fire as the engine's lifecycle progresses.

## Sources

- [PS2 Tek - Emotion Engine](https://psi-rockin.github.io/ps2tek/) — registry of every EE/IOP/GS register
- [ps2dev wiki](https://www.ps2dev.org/) — community SDK docs
- [PS2 Programming Manual Architecture](https://www.copetti.org/writings/consoles/playstation-2/) — Rodrigo Copetti's overview
