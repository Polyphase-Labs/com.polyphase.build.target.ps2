# PS2 Graphics — Polyphase Build Target

`Runtime/PS2/Graphics_PS2GS/Graphics_PS2GS.cpp` implements the engine's
`GFX_*` surface against gsKit + dmaKit. The PS2 GS has no programmable
shaders — every lighting, projection, and shading effect that would
normally happen in a fragment shader is done CPU-side on the EE and baked
into per-vertex color before the triangle is submitted.

## Display setup

NTSC 640×448 interlaced, 24-bit color + 16-bit Z. `Main_PS2.cpp::OctPreInitialize`
forces this resolution before `ReadEngineConfig` runs (`mWindowWidth/Height`
override). `OctPostInitialize` reapplies it because `ReadEngineConfig`
clobbers the values from `Config.ini`.

```
sGsGlobal->Mode             = GS_MODE_NTSC;
sGsGlobal->Width            = 640;
sGsGlobal->Height           = 448;
sGsGlobal->Interlace        = GS_INTERLACED;
sGsGlobal->Field            = GS_FIELD;
sGsGlobal->PSM              = GS_PSM_CT24;   // 24-bit color
sGsGlobal->PSMZ             = GS_PSMZ_16S;   // 16-bit signed depth
sGsGlobal->DoubleBuffering  = GS_SETTING_ON;
sGsGlobal->ZBuffering       = GS_SETTING_ON;
```

PAL (640×512) is wired in the GS register layout but not yet selected by
`EngineConfig`. Phase 3 will plumb region selection through.

## gsKit + dmaKit

| Library | Role |
| --- | --- |
| **gsKit** | High-level abstraction. Owns `GSGLOBAL` state, frame begin/end, primitive emit, queue management. |
| **dmaKit** | DMA-channel-level wrapper underneath. Must be initialised before gsKit's first `gsKit_init_screen`. |
| **gsToolkit** | Texture loaders (PNG, JPEG, BMP) — not used by the addon (engine loads textures via stb_image and uploads through gsKit's texture-manager). |

Init order in `InitGs`:

1. `gsKit_init_global_custom(4MB oneshot, 256KB persistent)` — bumped from
   default 1 MB oneshot because complex scenes can fill 1 MB at ~100 B per
   GIF triangle packet.
2. `dmaKit_init` + `dmaKit_chan_init(DMA_CHANNEL_GIF)` — opens the GIF DMA
   channel in `GIF_MODE_NORMAL` (non-chained DMA).
3. `gsKit_init_screen` — fires first DMA, must come after dmaKit.

## The two queues

gsKit maintains two queues:

| Queue | Size | Purpose |
| --- | --- | --- |
| **Persistent** | 256 KB | One-time state config (e.g. `gsKit_set_test`). Issued ONCE in `InitGs`; every `gsKit_queue_exec` re-dispatches it without clearing, so state persists between frames. |
| **Oneshot** | 4 MB | Per-frame `gsKit_clear` + draws. Cleared after every `gsKit_queue_exec`. |

**Mistake to avoid**: setting GS state (e.g. `GS_ZTEST_OFF`) while in
ONESHOT mode drops the state-write into the Oneshot queue, which clears
after frame 1's exec. Frames 2+ revert to GS defaults. Always write state
into the Persistent queue (`gsKit_mode_switch(GSKIT_PERSISTENT)` before,
`GSKIT_ONESHOT` after) for cross-frame configuration.

## Frame lifecycle

```
GFX_BeginFrame()
    gsKit_clear(sGsGlobal, kClearColor)
    (Oneshot queue receives the clear packet)

[engine emits draws via GFX_DrawStaticMesh / GFX_DrawTileMap2D /
 GFX_DrawParticle / GFX_DrawWidget / etc.]
    Each draw: CPU MVP transform per vertex, per-vertex Gouraud color
    (lit from World::GetMainLight + ambient), GIF packet emit via
    gsKit_prim_triangle_goraud_texture_3d.

GFX_EndFrame()
    gsKit_queue_exec(sGsGlobal)       // flush Persistent + Oneshot
    gsKit_sync_flip(sGsGlobal)        // wait for VSync + swap framebuffers
```

## The vertex pipeline

Polyphase passes `VertexBasic` / `VertexColor` / `VertexParticle` arrays
to the addon. PS2 has no vertex shaders, so transform is done EE-side:

```cpp
const glm::mat4 mvp = camera->GetViewProjectionMatrix() * model;
for each triangle (i0, i1, i2):
    const glm::vec4 p0 = mvp * glm::vec4(verts[i0].mPosition, 1.0f);
    if (p0.w <= 0.0f || p1.w <= 0.0f || p2.w <= 0.0f) continue;  // behind near plane
    const float invW0 = 1.0f / p0.w;
    const float nx0 = p0.x * invW0;    // NDC
    const float ny0 = p0.y * invW0;
    const float nz0 = p0.z * invW0;
    const float x0  = (nx0 * 0.5f + 0.5f) * kViewportW;        // screen x
    const float y0  = (1.0f - (ny0 * 0.5f + 0.5f)) * kViewportH;   // flipped y
    const int iz0   = (int)((1.0f - nz0) * 16383.5f);          // 16-bit GS Z
    // ... lighting: bake per-vertex N·L + ambient into RGBA8 ...
    gsKit_prim_triangle_goraud_texture_3d(...);
```

Notes:

- **Y is flipped** between NDC ([-1, +1] bottom-to-top) and GS screen
  ([0, 448] top-to-bottom). The `1.0f - (ny * 0.5f + 0.5f)` does that.
- **Z is inverted + scaled to 16-bit** (`(1 - nz) * 16383.5`). GS depth is
  "larger = closer" by convention when using `GS_ZTEST_GREATER`.
- **W-clip** (`if w <= 0`) drops triangles whose vertices are behind the
  camera. No proper homogeneous clipping yet — partially-clipped tris are
  drawn whole and may stretch. Acceptable for tile-based / billboard
  scenes; not great for fly-through cameras.

## Mesh classes

| Resource | Storage | Draw path |
| --- | --- | --- |
| **StaticMesh** | `std::unordered_map<StaticMesh*, ...>` per cooked mesh | Lit by main directional + ambient, optional texture (RGBA8 via `gsKit_TexManager_bind`), backface cull respects `MaterialLite::GetCullMode()`. |
| **TileMap2D** | `sTileMaps[TileMap2D*]` → `Ps2VertexColorMesh` (verts + indices) | Unlit, alpha-blend ON, no cull. Engine cooks a triangle mesh from the tile grid + atlas; we just transform + emit. |
| **Terrain3D** | `sTerrains[Terrain3D*]` → `Ps2VertexColorMesh` | Same shape as TileMap2D — engine pre-shades per-vertex color. |
| **Voxel3D** | `sVoxels[Voxel3D*]` → `Ps2VertexColorMesh` | Same shape. |
| **Particle3D** | `sParticleVerts[Particle3D*]` → `std::vector<VertexParticle>` (4 quad-corners) | Engine simulates particles CPU-side. We expand 4→6 verts per particle (two triangles, winding 0-1-2 / 2-1-3), apply optional `useLocalSpace` model matrix, emit unlit + alpha-blend. |
| **TextMesh3D / Skybox / Quad / Text / Poly widgets** | per-comp maps | Each renders through the same gsKit packet emit, just with different vertex layouts and lit-vs-unlit choices. |

All three vertex-color resource types (`TileMap2D`, `Terrain3D`, `Voxel3D`)
share the `DrawVertexColorMesh` helper and `GetVcMeshTexture` template
defined in the anonymous namespace above the per-type `GFX_*` functions.

## Lighting

`GatherMainLight(world)` walks the scene for a `DirectionalLight3d` and
returns a `PointLightish` struct (direction + color + ambient). The draw
helper then per-vertex computes `max(0, dot(N, lightDir)) * lightColor +
ambientColor`, baked into the GIF packet's RGBA. Material's
"Lit / Unlit / Toon" type-flag from `MaterialLite::GetShadingModel`
selects whether to compute the dot product or pass the pre-baked color
through.

Three-point lighting setups, fill lights, etc. are not supported — only
one directional + ambient. Multiple light sources would require either
multiple lighting passes (cost: doubled fillrate) or stacking per-vertex
contributions on the EE (cost: EE time linear in light-count × vert-count).
Acceptable for the engine's PSP-tier shading expectations.

## Textures

Polyphase uploads textures via `GFX_CreateTextureResource`. The PS2 path
stashes a `Ps2TextureData { GSTEXTURE mGsTex; GsKitClamp mClampMode; }`
in `sTextures[Texture*]` and lazily binds via `gsKit_TexManager_bind` on
first draw using that texture. gsKit handles VRAM cache management.

Hard limits:

| Limit | Value |
| --- | --- |
| Max size per mip | 1024 × 1024 (log2 0..10) |
| Width / height | Power-of-2 only — non-PoT not supported by the GS |
| Pixel formats | RGBA8 (`GS_PSM_CT32`), RGB888 (`GS_PSM_CT24`), CLUT8 (`GS_PSM_T8`) |
| Filtering | LINEAR (default in this addon) or NEAREST |
| Clamp / Repeat | Per-bind via `gsKit_set_clamp` |

Texture upload is "EE RAM → gsKit's host buffer → GIF DMA → VRAM" on
first bind; subsequent binds re-DMA from gsKit's host buffer until VRAM
pressure forces eviction.

## Common pitfalls

- **Y-flip mismatch** — if a font/widget renders upside-down, the
  `1.0f - (ny * 0.5f + 0.5f)` step is missing or doubled. UI widgets
  use a separate ortho path that also flips; don't double-flip.
- **Behind-near-plane geometry** — the `w <= 0` early-out drops whole
  triangles. A partially-clipped tri (one vert behind, two in front)
  stretches across the screen.
- **Persistent vs Oneshot queue** — state changes that need to survive
  frames must go into Persistent. Per-frame draws go into Oneshot. See
  the "two queues" section.
- **GIF packet overflow** — 4 MB Oneshot is generous, but a scene with
  > 30k textured triangles can fill it. `gsKit_init_global_custom` is
  parameterised at the top of `InitGs` if a project needs more.
- **VRAM cache thrashing** — gsKit_TexManager evicts on LRU. Drawing 50
  unique textures per frame causes constant DMAs from EE RAM. Group
  draws by texture to keep the working set in VRAM.

## Open work

- **PAL 640×512** — flip `GS_MODE_NTSC` → `GS_MODE_PAL` based on
  `EngineConfig` region. Refresh becomes 50 Hz.
- **Skeletal meshes** — Phase 4. Plan: CPU-skin on EE (mirror the PSP
  `project_psp_skeletal_mesh_cpu_skinning` pattern), repack to
  `VertexBasic`, draw through the static-mesh path.
- **Texture swizzling** — GS prefers tile-swizzled textures for fastest
  sampling. Cook-time swizzle pass would speed up texture-heavy scenes.
- **VU1 microcode draw path** — bypass gsKit's EE-side packet emit and
  upload static-mesh vertex data once to VU1 microcode. Substantial
  perf win for static geometry, but a major surgery.

## References

- [gsKit source](https://github.com/ps2dev/gsKit) — primary library
- [PS2 Tek — Graphics Synthesizer](https://psi-rockin.github.io/ps2tek/#gs) — GS register reference
- [Sony PS2 GS User's Guide](https://archive.org/details/PS2GraphicsSynthesizerUserManual) — official register-level docs
- [Copetti — PlayStation 2](https://www.copetti.org/writings/consoles/playstation-2/) — accessible architecture overview
