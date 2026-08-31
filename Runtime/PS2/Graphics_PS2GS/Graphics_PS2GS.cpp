/**
 * @file Graphics_PS2GS.cpp
 * @brief PS2 GS (Graphics Synthesizer) implementation of the engine's GFX_*
 *        surface, built on gsKit + dmaKit.
 *
 * Phase 0-2:
 *   - GFX_Initialize: bring up gsKit + dmaKit, NTSC 640x448 framebuffer, GS_PSM_CT24
 *     color + GS_PSMZ_16S Z, double-buffered with the GIF DMA channel inited
 *     in normal mode.
 *   - GFX_BeginFrame: clear the framebuffer to a known color (dark blue).
 *   - GFX_EndFrame: flush gsKit's queue + sync-flip.
 *   - GFX_DrawTriangleDemo: gouraud-shaded triangle. Wire into a test scene
 *     from OctPostUpdate to prove the pipeline.
 *   - Everything else (mesh / material / skeletal / particle resources +
 *     draws): empty stubs that return success so engine code links + runs.
 *
 * Phase 3+: real texture upload (with VRAM management), static-mesh draw via
 * gsKit_prim_list_triangle_*, skeletal-mesh CPU-skinning + repack (mirror PSP
 * project_psp_skeletal_mesh_cpu_skinning pattern), particle draw with 4→6
 * vertex expansion, UI widget rotation/scale.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Graphics/Graphics.h"
#include "Graphics/GraphicsTypes.h"
#include "Engine.h"
#include "Engine/World.h"
#include "Engine/Vertex.h"
#include "Engine/Assets/StaticMesh.h"
#include "Engine/Assets/SkeletalMesh.h"
#include "Engine/Assets/Texture.h"
#include "Engine/Assets/Material.h"
#include "Engine/Assets/MaterialLite.h"
#include "Engine/Assets/Font.h"
#include "Engine/Renderer.h"      // Renderer::Get()->GetDefaultMaterial()
#include "Engine/Nodes/3D/Camera3d.h"
#include "Engine/Nodes/3D/StaticMesh3d.h"
#include "Engine/Nodes/3D/Skybox3D.h"
#include "Engine/Nodes/3D/SkeletalMesh3d.h"
#include "Engine/Nodes/3D/InstancedMesh3d.h"
#include "Engine/Nodes/3D/Particle3d.h"
#include "Engine/Nodes/3D/DirectionalLight3d.h"
#include "Engine/Nodes/3D/PointLight3d.h"
#include "Engine/Nodes/3D/SpotLight3d.h"
#include "Engine/Nodes/Widgets/Quad.h"
#include "Engine/Nodes/Widgets/Text.h"
#include "Engine/Nodes/Widgets/Poly.h"
#include "Engine/Nodes/Widgets/Widget.h"
#include "Engine/Nodes/3D/TileMap2d.h"
#include "Engine/Nodes/3D/Terrain3d.h"
#include "Engine/Nodes/3D/Voxel3d.h"
#include "Engine/Nodes/3D/TextMesh3d.h"
#include "Maths.h"      // glm::perspective, glm::ortho, glm::radians
#include "Log.h"

#include <gsKit.h>
#include <gsInline.h>   // gsKit_heap_alloc, gsKit_float_to_int_* (not pulled by gsKit.h)
#include <dmaKit.h>
#include <gsToolkit.h>
#include <kernel.h>
#include <malloc.h>     // memalign (newlib's PS2 port)
#include <math.h>       // expf — exponential fog ramp

#include <unordered_map>

#include "PS2GSTypes.h"
#include "PS2GSUtils.h"

namespace
{
    GSGLOBAL* sGsGlobal = nullptr;

    // Scene fog, stashed by GFX_SetFog (called every frame from Renderer::BeginFrame)
    // and consumed as per-vertex GS hardware fog in the mesh draw helpers.
    FogSettings sFog;

    // ---- Vblank yield ------------------------------------------------------
    // gsKit_sync_flip busy-waits (gsKit_vsync_wait spins on the GS field bit)
    // for the rest of every frame — ~15 ms of EE spin at 60 Hz. On PCSX2 that
    // spinning EE starves the emulated IOP, so audsrv can't push audio to SPU2
    // fast enough and sound plays in ~15x slow-motion (the audsrv chain itself
    // is fine — proven by the startup drain self-test). Sleeping the EE on a
    // vblank-interrupt semaphore instead of spinning lets PCSX2 schedule the
    // IOP, so SPU2 drains at full rate. GFX_EndFrame WaitSema()s this, then
    // does gsKit's flip with FirstFrame forced so it skips its own busy-wait.
    int sVblankSema = -1;

    int VblankHandler(int /*cause*/)
    {
        if (sVblankSema >= 0) iSignalSema(sVblankSema);
        return 0;
    }

    void InstallVblankYield()
    {
        if (sVblankSema >= 0) return;   // install once
        ee_sema_t s = {};
        s.init_count = 0;
        s.max_count  = 1;
        s.option     = 0;
        sVblankSema = CreateSema(&s);
        if (sVblankSema < 0)
        {
            LogWarning("[PS2] vblank sema create failed (%d) — audio may stay choppy", sVblankSema);
            return;
        }
        AddIntcHandler(INTC_VBLANK_S, VblankHandler, 0);
        EnableIntc(INTC_VBLANK_S);
        LogDebug("[PS2] vblank-yield installed (sema=%d) — EE sleeps for vsync instead of spinning",
                 sVblankSema);
    }

    // Phase 0-2 hardcodes NTSC. Phase 3 will switch on EngineConfig region
    // (or a -DPS2_REGION_PAL macro) and pick GS_MODE_PAL / 640x512 instead.
    constexpr int kFbWidth  = 640;
    constexpr int kFbHeight = 448;

    void InitGs()
    {
        // Use custom queue sizes — gsKit's default Persistent queue is 256 KB
        // and Oneshot is 1 MB. With ~6000 triangles per frame at ~100 bytes
        // per gsKit_prim_triangle GIF packet, even Oneshot's 1 MB can fill
        // up under load. Bump Oneshot to 4 MB so we have headroom for
        // complex scenes; Persistent stays small since we mostly use Oneshot.
        constexpr int kOsQueueBytes  = 4 * 1024 * 1024;
        constexpr int kPerQueueBytes = 256 * 1024;
        sGsGlobal = gsKit_init_global_custom(kOsQueueBytes, kPerQueueBytes);
        if (sGsGlobal == nullptr)
        {
            LogError("[PS2] gsKit_init_global_custom returned nullptr");
            return;
        }

        sGsGlobal->Mode             = GS_MODE_NTSC;
        sGsGlobal->Width            = kFbWidth;
        sGsGlobal->Height           = kFbHeight;
        sGsGlobal->Interlace        = GS_INTERLACED;
        sGsGlobal->Field            = GS_FIELD;
        sGsGlobal->PSM              = GS_PSM_CT24;
        sGsGlobal->PSMZ             = GS_PSMZ_16S;
        sGsGlobal->DoubleBuffering  = GS_SETTING_ON;
        sGsGlobal->ZBuffering       = GS_SETTING_ON;
        sGsGlobal->PrimAlphaEnable  = GS_SETTING_ON;

        // dmaKit must be inited BEFORE gsKit_init_screen — that call
        // submits its first DMA. GIF_MODE_NORMAL = standard non-chained DMA.
        dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                    D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
        dmaKit_chan_init(DMA_CHANNEL_GIF);

        gsKit_init_screen(sGsGlobal);

        // Two-mode queue strategy:
        //   Persistent queue (small, kept across frames) — holds state
        //     config like gsKit_set_test. Issued ONCE here; every
        //     queue_exec re-dispatches it without clearing, so state stays
        //     between frames.
        //   Oneshot queue (big, cleared per exec) — holds the per-frame
        //     gsKit_clear + gsKit_prim_* draws.
        //
        // Mistake we hit on PS2: setting GS_ZTEST_OFF while in ONESHOT
        // mode dropped the command into the Oneshot queue, which cleared
        // after frame 1's exec. Frames 2+ reverted to GS-default Z-test
        // and every fragment failed → bright green "no draws" PCSX2
        // fallback display, even though the engine kept rendering happily.
        gsKit_mode_switch(sGsGlobal, GS_PERSISTENT);
        // Z-test ON: closer fragment wins. Test compare defaults to GEQUAL
        // (greater-equal); we map NDC z = -1 (near plane) → iz=32767 and
        // NDC z = +1 (far) → iz=0 so "closer to camera" produces larger
        // gsKit `iz` values, which satisfies the GEQUAL test.
        gsKit_set_test(sGsGlobal, GS_ZTEST_ON);
        // Alpha test stays OFF — gsKit's default ATST=GEQUAL with AREF=0x80
        // is a binary cutoff that kills antialiased font glyph edges
        // (anything below 50% alpha gets discarded → pixelated text).
        // The alpha blend equation below handles transparent pixels
        // correctly on its own: src_alpha=0 → output = dst (background).
        // Alpha blend: standard "source over destination" — output =
        // src*As + dst*(1-As). GS blend equation is (A-B)*C + D, with
        // A=Cs (source RGB), B=Cd (dest RGB), C=As (source alpha),
        // D=Cd (dest RGB), giving: (Cs-Cd)*As + Cd = Cs*As + Cd*(1-As).
        //
        // PerPixel=0 is critical — the gsKit arg name is misleading. It
        // actually drives the GS PABE register ("per-pixel alpha blend
        // enable"), which when ON makes blending CONDITIONAL on source
        // alpha bit 7. Alpha < 0x80 → blend disabled → output = src
        // (binary cutoff). That made font glyph backgrounds render as
        // solid colored blocks (alpha=0 outside glyph still wrote src).
        // PerPixel=0 disables PABE so the blend equation applies to every
        // fragment uniformly, letting alpha=0 → output = dst (transparent).
        gsKit_set_primalpha(sGsGlobal, GS_SETREG_ALPHA(0, 1, 0, 1, 0x80), 0);
        gsKit_mode_switch(sGsGlobal, GS_ONESHOT);

        // gsKit's texture manager keeps a per-frame ring of recently-bound
        // textures so it can evict cold ones from VRAM when allocations
        // overflow. Init here; nextFrame hook fires in GFX_EndFrame.
        gsKit_TexManager_init(sGsGlobal);

        // Clear BOTH framebuffers before the engine main loop kicks in.
        // Alpha 0x80 is gsKit's identity = full opacity — with the alpha
        // blend equation `(src - dst) * src_alpha + dst` now active for
        // font-mask transparency, alpha 0x00 in the clear color makes the
        // clear a NO-OP (src_alpha=0 → output=dst, previous frame stays).
        // That manifested as initial-boot flashing + ghost trails on
        // animated meshes + strobing of unlit prims against the static
        // grid background.
        const u64 kClearColor = GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0);
        for (int i = 0; i < 2; ++i)
        {
            gsKit_clear(sGsGlobal, kClearColor);
            gsKit_queue_exec(sGsGlobal);
            gsKit_sync_flip(sGsGlobal);
        }

        GetEngineState()->mSystem.mGsGlobal = sGsGlobal;

        // Install the vblank-yield interrupt so GFX_EndFrame can sleep the EE for
        // vsync instead of busy-spinning (keeps the emulated IOP / audsrv fed).
        InstallVblankYield();

        LogDebug("[PS2] gsKit initialised: %dx%d NTSC, GS_PSM_CT24, double-buffered "
                 "(both buffers seeded with clear color)",
                 sGsGlobal->Width, sGsGlobal->Height);
    }
}

void GFX_Initialize()
{
    InitGs();
}

void GFX_Shutdown()
{
    // gsKit_deinit_global is not strictly required — the BIOS reclaims VRAM
    // on ELF exit. Leaving the global pointer alone keeps post-shutdown
    // log calls (which might still reach SystemState) safe.
    sGsGlobal = nullptr;
    GetEngineState()->mSystem.mGsGlobal = nullptr;
}

void GFX_BeginFrame()
{
    if (sGsGlobal == nullptr) return;

    // Black clear. Scenes without a Skybox3D expect a black background;
    // scenes with one will overdraw this anyway, so black is the safe
    // default. Alpha 0x80 = full-opacity src so the clear actually wipes
    // the previous frame (alpha 0x00 would no-op under the active
    // (Cs-Cd)*As+Cd blend, causing ghost trails).
    gsKit_clear(sGsGlobal, GS_SETREG_RGBAQ(0x00, 0x00, 0x00, 0x80, 0));
}

void GFX_EndFrame()
{
    if (sGsGlobal == nullptr) return;
    gsKit_queue_exec(sGsGlobal);

    // Sleep the EE until vblank on the interrupt semaphore instead of letting
    // gsKit_sync_flip busy-spin the whole frame — that spin starves the emulated
    // IOP on PCSX2 and slows audsrv/SPU2 ~15x (see InstallVblankYield). We then do
    // gsKit's own flip, decomposed: gsKit_sync_flip == vsync_wait + display_buffer
    // + switch_context + setactive, so replacing only the vsync_wait with WaitSema
    // keeps rendering identical while yielding the EE.
    if (sVblankSema >= 0)
    {
        // Exact replica of gsKit_sync_flip with only gsKit_vsync_wait's busy-spin
        // replaced by the yielding WaitSema. gsKit_sync_flip toggles ONLY
        // ActiveBuffer — it does NOT touch PrimContext. (My earlier use of
        // gsKit_switch_context ALSO flipped PrimContext every frame, which swapped
        // the primitive context and made the 2D/UI path flash while 3D survived.)
        // Drain any stale/backlogged vblank signals first, THEN wait for the next
        // real vblank. Without the drain, a leftover signal (e.g. accumulated during
        // a scene-load pause, or produced faster than consumed) makes WaitSema return
        // instantly, so the EE free-runs at >60fps with ~0 idle and the IOP/audio
        // starves (post-load choppy audio).
        while (PollSema(sVblankSema) >= 0) { }
        WaitSema(sVblankSema);            // sleep until the NEXT vblank (feeds IOP/audio)

        if (sGsGlobal->DoubleBuffering == GS_SETTING_ON)
        {
            gsKit_display_buffer(sGsGlobal);   // GS_SET_DISPFB2: show the buffer we drew
            sGsGlobal->ActiveBuffer ^= 1;      // flip to the other buffer (ActiveBuffer only)
        }
        gsKit_setactive(sGsGlobal);            // write FRAME regs for the new active buffer
    }
    else
    {
        gsKit_sync_flip(sGsGlobal);       // fallback if the vblank sema failed to install
    }

    // Tell the texture manager we're starting a new frame — it bookkeeps
    // VRAM residency so cold textures get evicted before warm ones.
    gsKit_TexManager_nextFrame(sGsGlobal);

    int32_t& counter = GetEngineState()->mSystem.mGsFrameCounter;
    counter++;
    // Log once per second; also first 3 frames so we can confirm dt
    // converges from the init-time spike (frame 1 ~1.5s) to per-frame
    // (~16ms at 60 fps).
    if (counter <= 3 || counter % 60 == 1)
    {
        // LogDebug("[PS2] GFX_EndFrame counter=%d gameDt=%.4f realDt=%.4f",
        //          counter,
        //          GetEngineState()->mGameDeltaTime,
        //          GetEngineState()->mRealDeltaTime);
    }
}

// Phase 2 proof-of-life. Call from a debug hook (e.g. OctPostUpdate) to
// verify the pipeline works end-to-end. Phase 3+ replaces this with real
// static-mesh draws.
extern "C" void GFX_DrawTriangleDemo()
{
    if (sGsGlobal == nullptr) return;
    gsKit_prim_triangle_gouraud(sGsGlobal,
        100.0f, 100.0f,
        300.0f, 100.0f,
        200.0f, 300.0f,
        0,    /* iz — Z value in through-mode-style coordinates */
        GS_SETREG_RGBAQ(0xFF, 0x00, 0x00, 0x80, 0),
        GS_SETREG_RGBAQ(0x00, 0xFF, 0x00, 0x80, 0),
        GS_SETREG_RGBAQ(0x00, 0x00, 0xFF, 0x80, 0));
}

// ----- Screen / view / pass — Phase 0-2 no-ops ----------------------------
void GFX_BeginScreen(uint32_t /*screenIndex*/) {}
void GFX_BeginView(uint32_t /*viewIndex*/) {}
bool GFX_ShouldCullLights() { return true; }
void GFX_BeginRenderPass(RenderPassId /*pass*/) {}
void GFX_EndRenderPass() {}
void GFX_SetPipelineState(PipelineConfig /*config*/) {}
void GFX_SetViewport(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/, bool) {}
void GFX_SetScissor(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/, bool) {}
void GFX_SetFog(const FogSettings& fogSettings) { sFog = fogSettings; }
void GFX_DrawLines(const std::vector<Line>& /*lines*/) {}
void GFX_DrawFullscreen() {}
void GFX_ResizeWindow() {}
void GFX_Reset() {}
uint32_t GFX_GetNumViews() { return 1; }
void GFX_SetFrameRate(int32_t /*frameRate*/) {}
void GFX_PathTrace() {}
void GFX_BeginLightBake() {}
void GFX_UpdateLightBake() {}
void GFX_EndLightBake() {}
bool GFX_IsLightBakeInProgress() { return false; }
float GFX_GetLightBakeProgress() { return 0.0f; }
void GFX_EnableMaterials(bool /*enable*/) {}
void GFX_BeginGpuTimestamp(const char* /*name*/) {}
void GFX_EndGpuTimestamp(const char* /*name*/) {}

// ----- Resource creation — Phase 0-2 no-ops -------------------------------
// Engine creates resources for every asset; we must succeed silently so
// engine code paths don't crash. Phase 3+ adds real VRAM/RAM upload.
// ----- Texture storage ----------------------------------------------------
// One GSTEXTURE per engine Texture, with a 256-byte-aligned EE RAM buffer
// holding the RGBA8 pixels. gsKit's TexManager_bind handles VRAM upload +
// eviction (PS2 has only ~1.7 MB VRAM free after framebuffers, so textures
// are streamed in on demand). Engine's Texture asset stores raw RGBA8 in a
// std::vector<uint8_t>, which is byte-identical to PS2 PSM_CT32 storage on
// little-endian (R at lowest byte) so no swap needed.
namespace
{
    struct Ps2TextureData
    {
        GSTEXTURE  mGsTex     = {};
        u32*       mAligned   = nullptr; // memalign'd RAM buffer (u32* not uint32_t* on PS2 EE GCC)
        uint32_t   mSrcW      = 0;
        uint32_t   mSrcH      = 0;
        int        mClampMode = GS_CMODE_REPEAT;  // per-texture wrap, applied at draw time
    };

    std::unordered_map<Texture*, Ps2TextureData> sTextures;

    // Engine FilterType / WrapMode → gsKit equivalents.
    inline int FilterToGs(FilterType ft)
    {
        return (ft == FilterType::Nearest) ? GS_FILTER_NEAREST : GS_FILTER_LINEAR;
    }
    inline int WrapToGs(WrapMode wm)
    {
        switch (wm)
        {
            case WrapMode::Clamp:  return GS_CMODE_CLAMP;
            case WrapMode::Repeat: return GS_CMODE_REPEAT;
            case WrapMode::Mirror: return GS_CMODE_REPEAT;  // GS sampler has no mirror; fall back to repeat
            default:               return GS_CMODE_REPEAT;
        }
    }

    // gsKit needs the EE Mem pointer 256-byte aligned for DMA upload. We
    // allocate via memalign and free in destroy.
    Ps2TextureData* AllocTextureSlot(Texture* engineTex, uint32_t width, uint32_t height)
    {
        const uint32_t numBytes = width * height * 4;  // RGBA8
        u32* buf = (u32*)memalign(256, numBytes);
        if (buf == nullptr) return nullptr;

        Ps2TextureData& slot = sTextures[engineTex];
        slot.mAligned = buf;
        slot.mGsTex.Width    = width;
        slot.mGsTex.Height   = height;
        slot.mGsTex.PSM      = GS_PSM_CT32;
        slot.mGsTex.ClutPSM  = 0;
        slot.mGsTex.Mem      = buf;
        slot.mGsTex.Clut     = nullptr;
        slot.mGsTex.Vram     = 0;                          // TexManager allocates on first bind
        slot.mGsTex.VramClut = 0;
        slot.mGsTex.Filter   = GS_FILTER_LINEAR;
        slot.mGsTex.Delayed  = 1;                          // managed by TexManager
        gsKit_setup_tbw(&slot.mGsTex);                     // compute TBW from PSM + Width
        return &slot;
    }
}

void GFX_CreateTextureResource(Texture* texture, std::vector<uint8_t>& data)
{
    if (texture == nullptr) return;

    const uint32_t srcW = texture->GetWidth();
    const uint32_t srcH = texture->GetHeight();
    if (srcW == 0 || srcH == 0) return;

    const uint32_t srcBytes = srcW * srcH * 4;
    if (data.size() < srcBytes)
    {
        LogWarning("[PS2] CreateTextureResource: data size %u < expected %u for %ux%u",
                   (unsigned)data.size(), srcBytes, srcW, srcH);
        return;
    }

    // Two-part fix:
    //
    // 1. GS sampler clamps non-POT dims DOWN to next-lower POT silently
    //    (memory: project_psp_texture_pot_uvmax). 720×480 sampled as 512×256
    //    with content beyond 512 wrapping → mid-gray after lighting modulation.
    //
    // 2. PS2 VRAM is 4 MB total; framebuffer+Z eat ~2.87 MB so only ~1.13 MB
    //    is free for textures. The padded version of a 720×480 texture is
    //    1024×512×4 = 2 MB → doesn't fit. PSP/3DS/Wii sample from main RAM
    //    so they don't have this constraint. We cap textures at 512×512
    //    (1 MB max) and nearest-neighbour downsample anything bigger.
    // Defensive cap on the SOURCE size — if the engine reports an
    // implausibly large texture (eg 8K render target) just refuse it.
    // 4096×4096 RGBA8 = 64 MB which would blow EE RAM even before our
    // downsample. Skip and the mesh falls back to untextured rendering.
    if (srcW > 4096u || srcH > 4096u)
    {
        LogWarning("[PS2] CreateTextureResource: refusing oversize source %ux%u", srcW, srcH);
        return;
    }

    auto nextPow2 = [](uint32_t v) -> uint32_t {
        if (v <= 1u) return 1u;
        --v;
        v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
        return v + 1u;
    };
    uint32_t potW = nextPow2(srcW);
    uint32_t potH = nextPow2(srcH);
    // Min POT 16: memalign(256, smaller) is degenerate (alignment > size).
    // 16×16×4 = 1024 B bypasses the alignment-vs-size edge case.
    if (potW < 16u) potW = 16u;
    if (potH < 16u) potH = 16u;

    // VRAM-fit cap. Tightened from 512×512 → 256×256 because the FIRST
    // texture user observed crashing with 512-cap was still too big when
    // combined with everything else in VRAM. 256×256×4 = 256 KB; PS2 has
    // ~1.13 MB free VRAM after framebuffer+Z, so ~4 textures can coexist
    // before TexManager has to start evicting.
    constexpr uint32_t kMaxDim   = 256u;
    constexpr uint32_t kMaxBytes = 256u * 1024u;  // 256 KB
    while (potW > kMaxDim || potH > kMaxDim || (potW * potH * 4u) > kMaxBytes)
    {
        if (potW > 16u) potW >>= 1;
        if (potH > 16u) potH >>= 1;
        if (potW <= 16u && potH <= 16u) break;
    }

    Ps2TextureData* slot = AllocTextureSlot(texture, potW, potH);
    if (slot == nullptr)
    {
        LogError("[PS2] CreateTextureResource: memalign failed for %ux%u (POT %ux%u, %u bytes)",
                 srcW, srcH, potW, potH, potW * potH * 4);
        return;
    }
    slot->mSrcW = srcW;
    slot->mSrcH = srcH;
    // Honour engine-side filter / wrap settings from the Texture asset.
    slot->mGsTex.Filter = FilterToGs(texture->GetFilterType());
    slot->mClampMode    = WrapToGs(texture->GetWrapMode());

    // Nearest-neighbour resample from (srcW × srcH) into (potW × potH).
    // Three cases this handles cleanly:
    //   srcW == potW, srcH == potH → straight copy (POT-native textures).
    //   srcW <  potW, srcH <  potH → upsampling (just copy + replicate).
    //   srcW >  potW, srcH >  potH → downsampling (VRAM-cap path).
    //   Mixed-direction cases also work via the same step math.
    // Note: u32* (unsigned int*) vs uint32_t* (unsigned long*) are different
    // types on PS2 EE GCC even though both are 32-bit. mAligned is u32*, so
    // dst32 must also be u32* to avoid an implicit conversion error.
    const u32* src32 = reinterpret_cast<const u32*>(data.data());
    u32* dst32 = slot->mAligned;
    // Fixed-point 16.16 step so the sampling stays accurate on EE FPU.
    const uint32_t stepX = (srcW << 16) / potW;
    const uint32_t stepY = (srcH << 16) / potH;
    uint32_t sy_fp = 0;
    for (uint32_t y = 0; y < potH; ++y, sy_fp += stepY)
    {
        const uint32_t sy = sy_fp >> 16;
        const u32* srcRow = src32 + (sy < srcH ? sy : (srcH - 1)) * srcW;
        u32* dstRow = dst32 + y * potW;
        uint32_t sx_fp = 0;
        for (uint32_t x = 0; x < potW; ++x, sx_fp += stepX)
        {
            const uint32_t sx = sx_fp >> 16;
            dstRow[x] = srcRow[sx < srcW ? sx : (srcW - 1)];
        }
    }

    // UVMax — engine-facing crop signal. For resampled-to-fit textures
    // (srcW != potW visible-content size), we've filled the entire POT
    // buffer with content (no padding band), so UVMax = 1.0. For NPOT
    // padded-up cases we'd set < 1.0, but the downsample path eliminates
    // padding entirely.
    texture->SetUVMax(glm::vec2(1.0f, 1.0f));

    LogDebug("[PS2] CreateTextureResource '%s': src %ux%u → pot %ux%u (%u bytes)",
             texture->GetName().c_str(),
             srcW, srcH, potW, potH, potW * potH * 4);
}

void GFX_DestroyTextureResource(Texture* texture)
{
    if (texture == nullptr) return;
    auto it = sTextures.find(texture);
    if (it == sTextures.end()) return;
    if (it->second.mAligned != nullptr) free(it->second.mAligned);
    // Tell gsKit to evict from VRAM if currently bound there.
    gsKit_TexManager_invalidate(sGsGlobal, &it->second.mGsTex);
    sTextures.erase(it);
}

void GFX_UpdateTextureResourcePixels(Texture* texture, const uint8_t* src,
                                      uint32_t srcWidth, uint32_t srcHeight)
{
    if (texture == nullptr || src == nullptr) return;
    auto it = sTextures.find(texture);
    if (it == sTextures.end()) return;
    Ps2TextureData& slot = it->second;
    if (srcWidth != slot.mGsTex.Width || srcHeight != slot.mGsTex.Height) return;
    memcpy(slot.mAligned, src, srcWidth * srcHeight * 4);
    // Invalidate VRAM so next bind re-uploads.
    gsKit_TexManager_invalidate(sGsGlobal, &slot.mGsTex);
}

void GFX_CreateMaterialResource(Material* /*material*/) {}
void GFX_DestroyMaterialResource(Material* /*material*/) {}

// ----- Static mesh storage ------------------------------------------------
// gsKit doesn't have a vertex shader, so we CPU-transform engine vertices
// through (Projection * View * Model) per draw and submit each triangle via
// gsKit_prim_triangle_gouraud. Storage holds the raw engine-format vertex /
// index data on EE main RAM; transformed coords are submitted inline (gsKit
// copies args into its queue, so no aliasing concerns for the basic prim
// path).
namespace
{
    struct Ps2MeshData
    {
        std::vector<Vertex>      mVertices;       // engine layout (pos/uv/uv/normal)
        std::vector<VertexColor> mVerticesColor;  // engine layout with vertex color
        std::vector<IndexType>   mIndices;
        bool                     mHasColor = false;
    };

    // Maps engine StaticMesh* → our CPU geometry. Created in
    // GFX_CreateStaticMeshResource, destroyed in
    // GFX_DestroyStaticMeshResource, looked up in GFX_DrawStaticMeshComp.
    std::unordered_map<StaticMesh*, Ps2MeshData> sStaticMeshes;
}

void GFX_CreateStaticMeshResource(StaticMesh* staticMesh, bool hasColor,
                                   uint32_t numVertices, void* vertices,
                                   uint32_t numIndices, IndexType* indices)
{
    if (staticMesh == nullptr || vertices == nullptr || indices == nullptr) return;

    Ps2MeshData data;
    data.mHasColor = hasColor;
    if (hasColor)
    {
        data.mVerticesColor.assign(
            static_cast<VertexColor*>(vertices),
            static_cast<VertexColor*>(vertices) + numVertices);
    }
    else
    {
        data.mVertices.assign(
            static_cast<Vertex*>(vertices),
            static_cast<Vertex*>(vertices) + numVertices);
    }
    data.mIndices.assign(indices, indices + numIndices);

    sStaticMeshes[staticMesh] = std::move(data);

    LogDebug("[PS2] GFX_CreateStaticMeshResource: %u verts, %u indices, hasColor=%d",
             numVertices, numIndices, hasColor ? 1 : 0);
}

void GFX_DestroyStaticMeshResource(StaticMesh* staticMesh)
{
    if (staticMesh == nullptr) return;
    sStaticMeshes.erase(staticMesh);
}

// (Skeletal mesh resource fns defined further down with real impls.)

void GFX_CreateStaticMeshCompResource(StaticMesh3D* /*c*/) {}
void GFX_DestroyStaticMeshCompResource(StaticMesh3D* /*c*/) {}
void GFX_UpdateStaticMeshCompResourceColors(StaticMesh3D* /*c*/) {}
namespace
{
    // GS framebuffer dimensions. Phase 0-2 hardcodes NTSC.
    constexpr float kViewportW = 640.0f;
    constexpr float kViewportH = 448.0f;

    // Normal-to-color helper for first-pass debug shading. With no textures
    // and no lighting wired yet, we colour vertices by their normal so the
    // geometry is visible (different cube faces get different colours).
    inline u64 NormalToColor(const glm::vec3& n)
    {
        const u32 r = u32((n.x * 0.5f + 0.5f) * 255.0f) & 0xFF;
        const u32 g = u32((n.y * 0.5f + 0.5f) * 255.0f) & 0xFF;
        const u32 b = u32((n.z * 0.5f + 0.5f) * 255.0f) & 0xFF;
        return GS_SETREG_RGBAQ(r, g, b, 0x00, 0);
    }

    inline glm::vec3 GetVertexPosition(const Ps2MeshData& m, uint32_t i)
    {
        return m.mHasColor ? m.mVerticesColor[i].mPosition : m.mVertices[i].mPosition;
    }
    inline glm::vec3 GetVertexNormal(const Ps2MeshData& m, uint32_t i)
    {
        return m.mHasColor ? m.mVerticesColor[i].mNormal : m.mVertices[i].mNormal;
    }
    inline glm::vec2 GetVertexUV(const Ps2MeshData& m, uint32_t i)
    {
        return m.mHasColor ? m.mVerticesColor[i].mTexcoord0 : m.mVertices[i].mTexcoord0;
    }

    // Find the diffuse texture for a mesh comp.
    // Mirrors PSPGU.cpp:1254-1257 — use comp->GetMaterial() (effective
    // material: override OR mesh's default), fall back to the renderer's
    // default white material if null. Material::AsLite is the engine's
    // static downcast helper (no RTTI on PS2 EE GCC builds).
    Ps2TextureData* GetMeshTexture(StaticMesh3D* comp, StaticMesh* /*mesh*/)
    {
        Material* matBase = comp ? comp->GetMaterial() : nullptr;
        MaterialLite* mat = Material::AsLite(matBase ? matBase : Renderer::Get()->GetDefaultMaterial());
        if (mat == nullptr) return nullptr;

        Texture* tex = mat->GetTexture(0);  // slot 0 = diffuse
        if (tex == nullptr) return nullptr;

        auto it = sTextures.find(tex);
        if (it == sTextures.end())
        {
            // Diagnostic: log the first ~6 missed lookups so we can see
            // which textures the engine binds at draw time but never sent
            // through GFX_CreateTextureResource. ColorCheck / engine-built
            // textures are prime suspects.
            static int sMiss = 0;
            if (sMiss < 6)
            {
                LogDebug("[PS2] GetMeshTexture: miss tex=%p name='%s'",
                         (void*)tex, tex->GetName().c_str());
                sMiss++;
            }
            return nullptr;
        }
        return &it->second;
    }

    // Effective MaterialLite for any mesh comp: the comp's material override, else
    // its default, else the renderer's default white material. Same downcast pattern
    // as GetMeshTexture (Material::AsLite — no RTTI on PS2 EE GCC).
    template<typename T>
    MaterialLite* ResolveMaterialLite(T* comp)
    {
        Material* base = comp ? comp->GetMaterial() : nullptr;
        return Material::AsLite(base ? base : Renderer::Get()->GetDefaultMaterial());
    }
}

// ----- Lighting (CPU per-vertex Lambert) ----------------------------------
// PS2 GS has no built-in lighting. We bake ambient + one directional light +
// up to kMaxPointLights point lights into per-vertex modulation color CPU-side.
// Engine vertex normals are in model space; we transform by model[0..2][0..2]
// (no inverse-transpose — non-uniform scale will skew, acceptable for v1).
namespace
{
    // Max point lights evaluated per vertex on the EE. Point-lit scenes rarely
    // need more, and attenuation zeroes distant ones anyway. Kept small because
    // this is a CPU per-vertex loop (cost = verts × lights).
    constexpr int kMaxPointLights = 4;

    struct PointLightCPU
    {
        glm::vec3 mPos    = glm::vec3(0.0f);
        glm::vec3 mColor  = glm::vec3(0.0f);   // GetColor()*intensity (no colorScale)
        float     mRadius = 0.0f;
        // Spot cone (SpotLight3D). mCosOuter = -2 marks a plain point light, so the
        // cone factor stays 1 without a per-vertex branch on light type.
        glm::vec3 mSpotDir  = glm::vec3(0.0f, 0.0f, -1.0f);
        float     mCosInner = 1.0f;
        float     mCosOuter = -2.0f;
    };

    // Everything the CPU vertex shader needs for one frame: world ambient, one
    // directional light, and up to kMaxPointLights point lights.
    struct SceneLighting
    {
        glm::vec3     mAmbient   = glm::vec3(0.1f, 0.1f, 0.1f);  // DEFAULT_AMBIENT_LIGHT_COLOR
        glm::vec3     mDir       = glm::vec3(0.0f, -1.0f, 0.0f); // world-space travel direction
        glm::vec3     mDirColor  = glm::vec3(0.0f);
        bool          mHasDir    = false;
        PointLightCPU mPoints[kMaxPointLights];
        int           mNumPoints = 0;
    };

    SceneLighting GatherLighting(World* world)
    {
        SceneLighting out;
        if (world == nullptr) return out;
        // Ambient AS-IS, no colorScale (pre-scaling saturates to white —
        // project_psp_lighting_ambient_saturation).
        out.mAmbient = glm::vec3(world->GetAmbientLightColor());

        // Directional: first DirectionalLight3D. FindNode walks the node tree so it
        // finds Static-domain lights, which Renderer::GetLightData drops in packaged
        // (non-editor) builds. Color = GetColor()*intensity, NO colorScale (desktop
        // Forward.frag:188 applies none to lights; ×colorScale over-brightens → white).
        DirectionalLight3D* dl = world->FindNode<DirectionalLight3D>();
        if (dl != nullptr)
        {
            const glm::vec3 d = dl->GetDirection();
            if (glm::length(d) > 0.0001f) out.mDir = glm::normalize(d);
            out.mDirColor = glm::vec3(dl->GetColor()) * dl->GetIntensity();
            out.mHasDir   = true;
        }

        // Point lights: World::GetLights() is the raw, unculled light registry (all
        // domains). Cap at kMaxPointLights; range attenuation zeroes out-of-reach
        // ones, so a simple first-N selection is acceptable at PS2 scene scale.
        const std::vector<Light3D*>& lights = world->GetLights();
        for (uint32_t i = 0; i < lights.size() && out.mNumPoints < kMaxPointLights; ++i)
        {
            Light3D* L = lights[i];
            if (L == nullptr || !L->IsPointLight3D() || !L->IsVisible()) continue;
            PointLight3D* pl = L->As<PointLight3D>();
            if (pl == nullptr) continue;
            PointLightCPU& p = out.mPoints[out.mNumPoints++];
            p.mPos    = pl->GetWorldPosition();
            p.mColor  = glm::vec3(pl->GetColor()) * pl->GetIntensity();
            p.mRadius = pl->GetRadius();

            if (L->IsSpotLight3D())
            {
                SpotLight3D* sl = L->As<SpotLight3D>();
                const glm::vec3 sd = sl->GetDirection();
                if (glm::length(sd) > 0.0001f) p.mSpotDir = glm::normalize(sd);
                const float outer = glm::clamp(sl->GetOuterAngle(), 0.1f, 89.9f);
                const float inner = glm::clamp(sl->GetInnerAngle(), 0.0f, outer);
                p.mCosInner = cosf(glm::radians(inner));
                p.mCosOuter = cosf(glm::radians(outer));
            }
        }
        return out;
    }

    // HDR per-vertex lit color = ambient + directional Lambert + Σ point-light
    // Lambert with linear range attenuation (matches desktop Forward.frag:204
    // `1 - clamp(dist/radius)`). May exceed 1.0 — the caller carries it through the
    // GS overbright MODULATE. worldPos only matters when point lights are present.
    inline glm::vec3 ComputeLighting(const glm::vec3& normalWS,
                                     const glm::vec3& worldPos,
                                     const SceneLighting& L)
    {
        glm::vec3 col = L.mAmbient;
        if (L.mHasDir)
        {
            float nl = -glm::dot(normalWS, L.mDir);
            if (!(nl == nl)) nl = 0.0f;                          // NaN guard (bad normals)
            col += L.mDirColor * glm::clamp(nl, 0.0f, 1.0f);
        }
        for (int i = 0; i < L.mNumPoints; ++i)
        {
            const glm::vec3 toL  = L.mPoints[i].mPos - worldPos;
            const float     dist = glm::length(toL);
            const float     r    = L.mPoints[i].mRadius;
            float           atten = (r > 0.0001f) ? glm::clamp(1.0f - dist / r, 0.0f, 1.0f) : 0.0f;
            if (atten <= 0.0f) continue;
            const glm::vec3 toLDir = toL / glm::max(dist, 0.0001f);
            if (L.mPoints[i].mCosOuter > -1.5f)
            {
                // Spot cone: -toLDir points from the light to the vertex.
                const float coneDot = glm::dot(L.mPoints[i].mSpotDir, -toLDir);
                const float denom = glm::max(L.mPoints[i].mCosInner - L.mPoints[i].mCosOuter, 0.0001f);
                atten *= glm::clamp((coneDot - L.mPoints[i].mCosOuter) / denom, 0.0f, 1.0f);
                if (atten <= 0.0f) continue;
            }
            float nl = glm::dot(normalWS, toLDir);
            if (!(nl == nl)) nl = 0.0f;
            col += L.mPoints[i].mColor * (glm::clamp(nl, 0.0f, 1.0f) * atten);
        }
        return col;
    }

    // Pack an HDR modulation color (~[0,2]) into a GS RGBAQ via the overbright
    // MODULATE range (fragment = texel * Cf/128, Cf∈[0,255] → mod ∈[0,~2.0]). This
    // carries the HDR lighting range through so a bright ambient + directional does
    // NOT saturate to white and lose contrast. `alpha` is the raw GS alpha byte:
    // 0xFF forces opaque (opaque/masked path — texel-alpha-immune, kills the old
    // cube "lighting flicker"); opacity*128 for translucent/additive draws.
    inline u64 PackModColor(const glm::vec3& c, u32 alpha)
    {
        auto pk = [](float v) -> u32 { return (u32)glm::clamp((int)(v * 128.0f), 0, 255); };
        return GS_SETREG_RGBAQ(pk(c.r), pk(c.g), pk(c.b), alpha, 0);
    }

    // Unpack a packed RGBA8888 vertex color (R in low byte) to linear [0,1] RGB.
    inline glm::vec3 UnpackVertexColorRGB(uint32_t c)
    {
        return glm::vec3((c & 0xFF) / 255.0f,
                         ((c >> 8) & 0xFF) / 255.0f,
                         ((c >> 16) & 0xFF) / 255.0f);
    }

    // Same alpha-overbright trick — opaque pass-through for unlit 3D
    // meshes (skybox, debug grids).
    inline u64 UnlitModulationColor()
    {
        return GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0xFF, 0);
    }

    // Look up the material's shading model. Unlit → no Lambert in
    // DrawTrisHelper. Engine ships a `MaterialLite::GetShadingModel()` enum
    // with Lit / Unlit / Toon / Custom. PS2 only does the lit/unlit fork
    // for now; Toon / Custom render as Lit.
    bool IsCompUnlit(StaticMesh3D* comp)
    {
        if (comp == nullptr) return false;
        Material* matBase = comp->GetMaterial();
        MaterialLite* mat = Material::AsLite(matBase ? matBase : Renderer::Get()->GetDefaultMaterial());
        return (mat != nullptr) && (mat->GetShadingModel() == ShadingModel::Unlit);
    }
    bool IsCompUnlit(SkeletalMesh3D* comp)
    {
        if (comp == nullptr) return false;
        Material* matBase = comp->GetMaterial();
        MaterialLite* mat = Material::AsLite(matBase ? matBase : Renderer::Get()->GetDefaultMaterial());
        if (mat == nullptr) return false;
        return mat->GetShadingModel() == ShadingModel::Unlit;
    }
    bool IsCompUnlit(InstancedMesh3D* comp)
    {
        if (comp == nullptr) return false;
        Material* matBase = comp->GetMaterial();
        MaterialLite* mat = Material::AsLite(matBase ? matBase : Renderer::Get()->GetDefaultMaterial());
        if (mat == nullptr) return false;
        return mat->GetShadingModel() == ShadingModel::Unlit;
    }
}

// ----- GS hardware fog ----------------------------------------------------
// PS2 GS blends the post-texture fragment toward FOGCOL by (1 - F/255), where F
// is a per-vertex coefficient carried in XYZF2 and the PRIM FGE bit enables it.
// gsKit's prim helpers only emit XYZ2 (no F), so we replicate their GIF packets
// (RGBAQ [+ UV] + XYZF2) verbatim from gsPrimitive.c / gsTexture.c, substituting
// XYZF2 for XYZ2 and providing the fog coefficient. Works on textured meshes,
// which per-vertex modulation color cannot fog.
namespace
{
    // Unique GIF batch types so fog triangles never merge with non-fog prims.
    constexpr int kGifPrimTriGouraudFog = 0x1801;   // vs GSKIT_GIF_PRIM_TRIANGLE_GOURAUD 0x1800

    // REGLISTs = gsKit's, with GS_XYZF2 in place of GS_XYZ2.
    inline u64 FogGouraudRegs()
    {
        return ((u64)(GS_PRIM)  << 0)  | ((u64)(GS_RGBAQ) << 4)  |
               ((u64)(GS_XYZF2) << 8)  | ((u64)(GS_RGBAQ) << 12) |
               ((u64)(GS_XYZF2) << 16) | ((u64)(GS_RGBAQ) << 20) |
               ((u64)(GS_XYZF2) << 24) | ((u64)(GIF_NOP)  << 28);
    }
    inline u64 FogTexGouraudRegs(int ctx)
    {
        return ((u64)(GS_TEX0_1 + ctx) << 0)  | ((u64)(GS_PRIM)  << 4)  |
               ((u64)(GS_RGBAQ)        << 8)  | ((u64)(GS_UV)     << 12) |
               ((u64)(GS_XYZF2)        << 16) | ((u64)(GS_RGBAQ)  << 20) |
               ((u64)(GS_UV)           << 24) | ((u64)(GS_XYZF2)  << 28) |
               ((u64)(GS_RGBAQ)        << 32) | ((u64)(GS_UV)     << 36) |
               ((u64)(GS_XYZF2)        << 40) | ((u64)(GIF_NOP)   << 44);
    }

    // Per-vertex fog coefficient F (0=full fog .. 255=clear) from eye-space depth.
    inline int FogCoefficient(float depth)
    {
        const float span = sFog.mFar - sFog.mNear;
        float t;
        if (span <= 0.0001f)
        {
            t = (depth >= sFog.mFar) ? 1.0f : 0.0f;
        }
        else if (sFog.mDensityFunc == FogDensityFunc::Exponential)
        {
            // No authored density scalar; shape an exp ramp from near/far so
            // t≈0.95 at far (exp(-3)≈0.05). Matches the C3D/GX spirit.
            const float d = (depth < sFog.mNear) ? 0.0f : (depth - sFog.mNear);
            t = 1.0f - expf(-(3.0f / span) * d);
        }
        else
        {
            t = (depth - sFog.mNear) / span;   // Linear
        }
        t = glm::clamp(t, 0.0f, 1.0f);
        t *= sFog.mColor.a;                       // fog-color alpha scales overall density (matches Fog.glsl)
        return (int)((1.0f - t) * 255.0f);
    }

    // Skybox fog coefficient — horizon gradient, NOT distance (mirrors Fog.glsl
    // ApplyFogSky). Distance fog on the sky is meaningless (it sits far past the fog
    // Far plane, so distance fog would flood the whole dome one flat color and the
    // sky vanishes). Instead fade by the view-ray elevation: full fog at/below the
    // horizon, clearing toward the zenith — so fogged distant terrain blends
    // seamlessly into the sky. `localPos` is the skybox vertex in model space.
    inline int SkyFogCoefficient(const glm::mat4& model, const glm::vec3& localPos,
                                 const glm::vec3& camPos)
    {
        const glm::vec3 worldPos = glm::vec3(model * glm::vec4(localPos, 1.0f));
        const glm::vec3 d = worldPos - camPos;
        const float len = glm::length(d);
        const float dy = (len > 1e-6f) ? (d.y / len) : 1.0f;   // normalized elevation, ~0 at horizon
        constexpr float kHorizonFalloff = 0.25f;               // ~14° fade band above the horizon
        const float t = glm::clamp(dy / kHorizonFalloff, 0.0f, 1.0f);
        const float s = t * t * (3.0f - 2.0f * t);             // smoothstep(0,1,t)
        const float fogFactor = (1.0f - s) * sFog.mColor.a;    // 1 = full fog (horizon), 0 = clear (zenith)
        return (int)((1.0f - fogFactor) * 255.0f);             // F: 0 = full fog, 255 = clear
    }

    // log2 (rounded up) of a texture dimension, for the TEX0 TW/TH fields.
    // gsKit's own gsKit_set_tw_th is static/private, so we compute it (engine
    // PS2 textures are already power-of-two, so this is exact).
    inline int TexLog2(int v)
    {
        int r = 0;
        while ((1 << r) < v) ++r;
        return r;
    }

    // Program FOGCOL (register 0x3d) via a queued A+D packet. Cheap; call once
    // before a fog-enabled mesh's triangles.
    inline void WriteFogColor(const glm::vec4& c)
    {
        const u32 r = (u32)(glm::clamp(c.r, 0.0f, 1.0f) * 255.0f);
        const u32 g = (u32)(glm::clamp(c.g, 0.0f, 1.0f) * 255.0f);
        const u32 b = (u32)(glm::clamp(c.b, 0.0f, 1.0f) * 255.0f);
        u64* p = (u64*)gsKit_heap_alloc(sGsGlobal, 1, 16, GIF_AD);
        *p++ = GIF_TAG_AD(1);
        *p++ = GIF_AD;
        *p++ = ((u64)r | ((u64)g << 8) | ((u64)b << 16));   // FOGCOL value
        *p++ = GS_FOGCOL;                                    // register address
    }

    // Toggle GS depth writes via the ZBUF register's ZMSK bit (1 = mask/no write,
    // 0 = write). Translucent/additive draws set ZMSK=1 so they don't occlude each
    // other or geometry drawn after them (they still depth-TEST). We rebuild ZBUF
    // from gsKit's own ZBuffer/PSMZ using its 8 KB-page convention (ZBP =
    // byteOffset/8192) so the base pointer matches whatever gsKit programmed.
    inline void WriteZWriteMask(int zmsk)
    {
        u64* p = (u64*)gsKit_heap_alloc(sGsGlobal, 1, 16, GIF_AD);
        *p++ = GIF_TAG_AD(1);
        *p++ = GIF_AD;
        *p++ = GS_SETREG_ZBUF(sGsGlobal->ZBuffer / 8192, sGsGlobal->PSMZ, zmsk);
        *p++ = GS_ZBUF_1 + sGsGlobal->PrimContext;          // 0x4e ctx1 / 0x4f ctx2
    }

    // Fog variant of gsKit_prim_triangle_gouraud_3d (untextured).
    void PrimTriGouraudFog(float x1, float y1, int iz1, int f1,
                           float x2, float y2, int iz2, int f2,
                           float x3, float y3, int iz3, int f3,
                           u64 c1, u64 c2, u64 c3)
    {
        const int ix1 = gsKit_float_to_int_x(sGsGlobal, x1), iy1 = gsKit_float_to_int_y(sGsGlobal, y1);
        const int ix2 = gsKit_float_to_int_x(sGsGlobal, x2), iy2 = gsKit_float_to_int_y(sGsGlobal, y2);
        const int ix3 = gsKit_float_to_int_x(sGsGlobal, x3), iy3 = gsKit_float_to_int_y(sGsGlobal, y3);

        u64* p_store;
        u64* p_data;
        p_store = p_data = (u64*)gsKit_heap_alloc(sGsGlobal, 4, 64, kGifPrimTriGouraudFog);
        if (p_store == (u64*)sGsGlobal->CurQueue->last_tag)
        {
            *p_data++ = GIF_TAG_TRIANGLE_GOURAUD(0);
            *p_data++ = FogGouraudRegs();
        }
        *p_data++ = GS_SETREG_PRIM(GS_PRIM_PRIM_TRIANGLE, 1, 0,
            sGsGlobal->PrimFogEnable, sGsGlobal->PrimAlphaEnable,
            sGsGlobal->PrimAAEnable, 0, sGsGlobal->PrimContext, 0);
        *p_data++ = c1; *p_data++ = GS_SETREG_XYZF2(ix1, iy1, iz1, f1);
        *p_data++ = c2; *p_data++ = GS_SETREG_XYZF2(ix2, iy2, iz2, f2);
        *p_data++ = c3; *p_data++ = GS_SETREG_XYZF2(ix3, iy3, iz3, f3);
    }

    // Fog variant of gsKit_prim_triangle_goraud_texture_3d. gsKit_set_texfilter
    // (as in the original) resets last_type so the always-written tag is correct.
    void PrimTriTexGouraudFog(GSTEXTURE* tex,
        float x1, float y1, int iz1, float u1, float v1, int f1,
        float x2, float y2, int iz2, float u2, float v2, int f2,
        float x3, float y3, int iz3, float u3, float v3, int f3,
        u64 c1, u64 c2, u64 c3)
    {
        gsKit_set_texfilter(sGsGlobal, tex->Filter);
        const int tw = TexLog2(tex->Width);
        const int th = TexLog2(tex->Height);

        const int ix1 = gsKit_float_to_int_x(sGsGlobal, x1), iy1 = gsKit_float_to_int_y(sGsGlobal, y1);
        const int ix2 = gsKit_float_to_int_x(sGsGlobal, x2), iy2 = gsKit_float_to_int_y(sGsGlobal, y2);
        const int ix3 = gsKit_float_to_int_x(sGsGlobal, x3), iy3 = gsKit_float_to_int_y(sGsGlobal, y3);
        const int iu1 = gsKit_float_to_int_u(tex, u1), iv1 = gsKit_float_to_int_v(tex, v1);
        const int iu2 = gsKit_float_to_int_u(tex, u2), iv2 = gsKit_float_to_int_v(tex, v2);
        const int iu3 = gsKit_float_to_int_u(tex, u3), iv3 = gsKit_float_to_int_v(tex, v3);

        u64* p_data = (u64*)gsKit_heap_alloc(sGsGlobal, 6, 96, GSKIT_GIF_PRIM_TRIANGLE_TEXTURED);
        *p_data++ = GIF_TAG_TRIANGLE_GORAUD_TEXTURED(0);
        *p_data++ = FogTexGouraudRegs(sGsGlobal->PrimContext);
        // TCC=1 (use TEXTURE alpha, not just RGB). Required for masked alpha-test
        // cutout so the texel's per-pixel alpha reaches the alpha test. Harmless for
        // opaque (alpha is unused when both blend and alpha-test are off) and already
        // needed by translucent. Was PrimAlphaEnable, which is OFF for masked → the
        // texel alpha was dropped and cutouts rendered solid.
        if (tex->VramClut == 0)
        {
            *p_data++ = GS_SETREG_TEX0(tex->Vram / 256, tex->TBW, tex->PSM, tw, th,
                1, 0, 0, 0, 0, 0, GS_CLUT_STOREMODE_NOLOAD);
        }
        else
        {
            *p_data++ = GS_SETREG_TEX0(tex->Vram / 256, tex->TBW, tex->PSM, tw, th,
                1, 0, tex->VramClut / 256, tex->ClutPSM,
                tex->ClutStorageMode, 0, GS_CLUT_STOREMODE_LOAD);
        }
        *p_data++ = GS_SETREG_PRIM(GS_PRIM_PRIM_TRIANGLE, 1, 1,
            sGsGlobal->PrimFogEnable, sGsGlobal->PrimAlphaEnable,
            sGsGlobal->PrimAAEnable, 1, sGsGlobal->PrimContext, 0);
        *p_data++ = c1; *p_data++ = GS_SETREG_UV(iu1, iv1); *p_data++ = GS_SETREG_XYZF2(ix1, iy1, iz1, f1);
        *p_data++ = c2; *p_data++ = GS_SETREG_UV(iu2, iv2); *p_data++ = GS_SETREG_XYZF2(ix2, iy2, iz2, f2);
        *p_data++ = c3; *p_data++ = GS_SETREG_UV(iu3, iv3); *p_data++ = GS_SETREG_XYZF2(ix3, iy3, iz3, f3);
    }
}

// ----- Helper: transform + cull + submit a triangle-list mesh -------------
// Used by static, instanced, and skeletal mesh draws — all three share the
// same vertex→screen pipeline post-skinning/transforms.
namespace
{
    void DrawTrisHelper(const std::vector<Vertex>& verts,
                        const std::vector<IndexType>& indices,
                        const glm::mat4& model,
                        const glm::mat4& mvp,
                        Ps2TextureData* texSlot,
                        const SceneLighting& light,
                        bool unlit,
                        bool invertCull,
                        MaterialLite* mat = nullptr,
                        const std::vector<VertexColor>* colorVerts = nullptr)
    {
        // Vertex source: either the plain Vertex vector (static/skeletal/instanced/
        // text) or a VertexColor vector (vertex-colored static meshes). hasColor
        // meshes were previously invisible because they populate mVerticesColor but
        // this helper only ever read `verts` (empty for them). Route both through
        // per-index accessors so both draw and colored verts can modulate.
        const bool   hasColor = (colorVerts != nullptr);
        const size_t vcount   = hasColor ? colorVerts->size() : verts.size();
        if (sGsGlobal == nullptr || vcount == 0 || indices.empty()) return;

        auto vPos = [&](uint32_t i) -> glm::vec3 { return hasColor ? (*colorVerts)[i].mPosition  : verts[i].mPosition; };
        auto vNrm = [&](uint32_t i) -> glm::vec3 { return hasColor ? (*colorVerts)[i].mNormal    : verts[i].mNormal; };
        auto vUV  = [&](uint32_t i) -> glm::vec2 { return hasColor ? (*colorVerts)[i].mTexcoord0 : verts[i].mTexcoord0; };
        auto vCol = [&](uint32_t i) -> uint32_t  { return hasColor ? (*colorVerts)[i].mColor     : 0xFFFFFFFFu; };

        // Material render state (tint/emissive/blend/cull/vertex-color). Null → the
        // engine default (white opaque, back-face cull, no emissive/vertex color).
        glm::vec3 matTint(1.0f);
        float     matEmission = 0.0f;
        BlendMode blend = BlendMode::Opaque;
        CullMode  cull  = CullMode::Back;
        bool      vcModulate = false;
        if (mat != nullptr)
        {
            matTint     = glm::vec3(mat->GetColor());
            matEmission = mat->GetEmission();
            blend       = mat->GetBlendMode();
            cull        = mat->GetCullMode();
            vcModulate  = (mat->GetVertexColorMode() != VertexColorMode::None) && hasColor;
        }
        const bool  translucent = (blend == BlendMode::Translucent || blend == BlendMode::Additive);
        const bool  masked      = (blend == BlendMode::Masked);
        const float opacity     = (translucent && mat != nullptr) ? mat->GetOpacity() : 1.0f;
        // Alpha byte fed to every vertex:
        //   Translucent/Additive → opacity*128 so the GS blend sees As = opacity.
        //   Masked → 0x80 (identity) so the fragment alpha == TEXEL alpha and the
        //     alpha test compares the texel's real alpha against the cutoff (0xFF
        //     overbright would push every non-zero texel past the cutoff).
        //   Opaque → 0xFF (overbright = force opaque, texel-alpha-immune; kills the
        //     cube "lighting flicker").
        const u32 alphaByte = translucent
            ? (u32)glm::clamp((int)(opacity * 128.0f), 0, 255)
            : (masked ? 0x80u : 0xFFu);

        // Blend enable (ABE bit in PRIM, read by gsKit AND our fog packets from
        // sGsGlobal->PrimAlphaEnable). Opaque/Masked stay unblended; the GS ALPHA
        // equation for translucent vs additive is selected below.
        sGsGlobal->PrimAlphaEnable = translucent ? GS_SETTING_ON : GS_SETTING_OFF;
        if (translucent)
        {
            // Translucent: src-over (Cs-Cd)*As+Cd. Additive: Cs*As+Cd (B=0/"2", D=Cd).
            const u64 alphaReg = (blend == BlendMode::Additive)
                ? GS_SETREG_ALPHA(0, 2, 0, 1, 0x80)   // Cs*As + Cd
                : GS_SETREG_ALPHA(0, 1, 0, 1, 0x80);  // (Cs-Cd)*As + Cd
            gsKit_set_primalpha(sGsGlobal, alphaReg, 0);
            // No depth writes for blended draws (they still depth-TEST against opaque)
            // — otherwise overlapping/back-to-front translucency self-occludes.
            WriteZWriteMask(1);
        }
        // Masked (alpha-test cutout): discard texels below the mask cutoff so cutout
        // textures (foliage, chain-link, decals) show holes instead of rendering the
        // transparent regions solid. gsKit_set_test manages the Z fields, so this
        // only flips the alpha-test enable — restored to OFF at function end (the UI
        // font path needs alpha test OFF for antialiased glyph edges).
        if (masked)
        {
            sGsGlobal->Test->ATST  = 5;   // GEQUAL: keep fragment if alpha >= AREF
            sGsGlobal->Test->AREF  = (u8)glm::clamp(
                (int)((mat != nullptr ? mat->GetMaskCutoff() : 0.5f) * 255.0f), 0, 255);
            sGsGlobal->Test->AFAIL = 0;   // KEEP: a failed pixel updates nothing
            gsKit_set_test(sGsGlobal, GS_ATEST_ON);
        }

        // Fog: enable the PRIM FGE bit for this draw (per-vertex F below) and set
        // the fog color once. gsKit reads PrimFogEnable when emitting PRIM, so the
        // non-fog gsKit prim path must see it OFF (else stale F fogs everything).
        // The skybox (invertCull) gets fog too, but via a HORIZON gradient
        // (SkyFogCoefficient) instead of distance — distance fog on the sky floods
        // the whole dome flat and the sky vanishes. Horizon fog fades the sky to the
        // fog color at the horizon so fogged distant geometry blends into it.
        const bool fog    = sFog.mEnabled;
        const bool skyFog = fog && invertCull;
        sGsGlobal->PrimFogEnable = fog ? GS_SETTING_ON : GS_SETTING_OFF;
        glm::vec3 fogCamPos(0.0f);
        if (fog)
        {
            WriteFogColor(sFog.mColor);
            if (skyFog)
            {
                World* fw = GetWorld(0);
                Camera3D* fc = fw ? fw->GetActiveCamera() : nullptr;
                if (fc != nullptr) fogCamPos = fc->GetWorldPosition();
            }
        }

        // Bind texture once for the whole mesh, and apply its wrap mode
        // (engine Texture::GetWrapMode → gsKit GS_CMODE_*). gsKit's clamp
        // setting is global state, so we set it per-draw to match the
        // active texture.
        if (texSlot != nullptr)
        {
            gsKit_TexManager_bind(sGsGlobal, &texSlot->mGsTex);
            gsKit_set_clamp(sGsGlobal, texSlot->mClampMode);
        }
        const float texW = texSlot ? (float)texSlot->mGsTex.Width  : 1.0f;
        const float texH = texSlot ? (float)texSlot->mGsTex.Height : 1.0f;

        // Normal-transform matrix = upper-left 3x3 of model. Skip inverse-
        // transpose — acceptable when scales are uniform (true for ~all
        // engine assets). Re-normalize per vertex.
        glm::mat3 normalMat(model);
        // If the engine handed us a NaN model matrix this frame (observed
        // on animated meshes — likely transform-cache invalidation in the
        // middle of HeroSpinner's Tick), fall back to identity so the
        // Lambert calc stays sane. Cheap one-cell sniff suffices.
        if (!(normalMat[0][0] == normalMat[0][0]))
        {
            normalMat = glm::mat3(1.0f);
        }

        const uint32_t numTris = (uint32_t)indices.size() / 3;
        for (uint32_t t = 0; t < numTris; ++t)
        {
            const uint32_t i0 = indices[t * 3 + 0];
            const uint32_t i1 = indices[t * 3 + 1];
            const uint32_t i2 = indices[t * 3 + 2];

            const glm::vec3 lp0 = vPos(i0), lp1 = vPos(i1), lp2 = vPos(i2);
            const glm::vec4 p0 = mvp * glm::vec4(lp0, 1.0f);
            const glm::vec4 p1 = mvp * glm::vec4(lp1, 1.0f);
            const glm::vec4 p2 = mvp * glm::vec4(lp2, 1.0f);
            if (p0.w <= 0.0f || p1.w <= 0.0f || p2.w <= 0.0f) continue;

            const float invW0 = 1.0f / p0.w, invW1 = 1.0f / p1.w, invW2 = 1.0f / p2.w;
            const float nx0 = p0.x * invW0, ny0 = p0.y * invW0, nz0 = p0.z * invW0;
            const float nx1 = p1.x * invW1, ny1 = p1.y * invW1, nz1 = p1.z * invW1;
            const float nx2 = p2.x * invW2, ny2 = p2.y * invW2, nz2 = p2.z * invW2;

            const float x0 = (nx0 * 0.5f + 0.5f) * kViewportW;
            const float y0 = (1.0f - (ny0 * 0.5f + 0.5f)) * kViewportH;
            const float x1 = (nx1 * 0.5f + 0.5f) * kViewportW;
            const float y1 = (1.0f - (ny1 * 0.5f + 0.5f)) * kViewportH;
            const float x2 = (nx2 * 0.5f + 0.5f) * kViewportW;
            const float y2 = (1.0f - (ny2 * 0.5f + 0.5f)) * kViewportH;

            // CW front-face in screen (engine is CW after Y-flip). Skybox
            // meshes have inverted normals (camera is INSIDE the mesh), so
            // the visible faces have flipped winding — invert the cull
            // condition to render the inward-facing side.
            //
            // EPSILON DEAD-BAND: triangles whose signed area is right
            // around 0 are edge-on to the camera. PS2's non-IEC559 FPU has
            // limited precision, so on an animated mesh the sign of the
            // computed area can flip frame-to-frame across the 0 boundary,
            // toggling triangles between drawn and culled. That manifests
            // as lighting "blinking on and off" because cube faces
            // momentarily disappear when they're edge-on. Cull anything
            // within ±0.5 px² of zero; visually those triangles cover less
            // than a pixel anyway.
            // Two-sided materials (CullMode::None) skip the test entirely.
            // Skybox: invertCull already selects the inward-facing winding — and the
            // engine ALSO tags the sky material CullMode::Front for the same effect
            // on desktop (Mesh3d.cpp), so on PS2 we must NOT apply both or they
            // cancel and the sky culls to nothing (black). invertCull wins for the
            // sky; otherwise honor the material cull mode (Front inverts back-cull).
            if (cull != CullMode::None || invertCull)
            {
                const bool invert = invertCull ? true : (cull == CullMode::Front);
                const float signedArea = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
                constexpr float kCullEpsilon = 0.5f;
                if (invert
                    ? (signedArea <=  kCullEpsilon)
                    : (signedArea >= -kCullEpsilon))
                {
                    continue;
                }
            }

            const int iz0 = (int)((1.0f - nz0) * 16383.5f);
            const int iz1 = (int)((1.0f - nz1) * 16383.5f);
            const int iz2 = (int)((1.0f - nz2) * 16383.5f);

            // Per-vertex modulation color = materialTint * lighting + emissive,
            // optionally × the baked vertex color. Unlit meshes skip Lambert
            // (lighting == 1). The result is packed through the GS overbright range.
            // Emissive (MaterialLite::GetEmission scalar) is an additive self-lit
            // term = tint*emission, so it shows even in shadow / on unlit materials.
            auto shade = [&](uint32_t idx, const glm::vec3& nWS, const glm::vec3& wPos) -> u64 {
                const glm::vec3 lit = unlit ? glm::vec3(1.0f)
                                            : ComputeLighting(nWS, wPos, light);
                glm::vec3 modc = matTint * lit + matTint * matEmission;
                if (vcModulate) modc *= UnpackVertexColorRGB(vCol(idx));
                return PackModColor(modc, alphaByte);
            };

            u64 c0, c1, c2;
            if (unlit)
            {
                // No Lambert — but still honor tint / emissive / vertex color.
                c0 = shade(i0, glm::vec3(0.0f), glm::vec3(0.0f));
                c1 = shade(i1, glm::vec3(0.0f), glm::vec3(0.0f));
                c2 = shade(i2, glm::vec3(0.0f), glm::vec3(0.0f));
            }
            else
            {
                // NaN-safe normalize: a degenerate model row at animation extremes
                // (e.g. HeroSpinner sin-wave touching 0 scale) collapses normalMat ×
                // normal to (0,0,0) → glm::normalize NaN → junk RGBAQ. Fall back to
                // "up"; visually indistinguishable on a single-frame transient.
                auto safeNormalize = [&](const glm::vec3& v) -> glm::vec3 {
                    const float len2 = glm::dot(v, v);
                    return (len2 > 1e-12f) ? (v / glm::sqrt(len2))
                                           : glm::vec3(0.0f, 1.0f, 0.0f);
                };
                const glm::vec3 n0 = safeNormalize(normalMat * vNrm(i0));
                const glm::vec3 n1 = safeNormalize(normalMat * vNrm(i1));
                const glm::vec3 n2 = safeNormalize(normalMat * vNrm(i2));
                // World positions only needed for point-light distance.
                const bool needWP = (light.mNumPoints > 0);
                const glm::vec3 w0 = needWP ? glm::vec3(model * glm::vec4(lp0, 1.0f)) : glm::vec3(0.0f);
                const glm::vec3 w1 = needWP ? glm::vec3(model * glm::vec4(lp1, 1.0f)) : glm::vec3(0.0f);
                const glm::vec3 w2 = needWP ? glm::vec3(model * glm::vec4(lp2, 1.0f)) : glm::vec3(0.0f);
                c0 = shade(i0, n0, w0);
                c1 = shade(i1, n1, w1);
                c2 = shade(i2, n2, w2);
            }

            // Per-vertex fog coefficient. 255 = clear (no fog / near), 0 = full fog.
            // Skybox uses the horizon gradient (elevation); everything else uses
            // eye-space distance (clip w).
            int f0 = 255, f1 = 255, f2 = 255;
            if (skyFog)
            {
                f0 = SkyFogCoefficient(model, lp0, fogCamPos);
                f1 = SkyFogCoefficient(model, lp1, fogCamPos);
                f2 = SkyFogCoefficient(model, lp2, fogCamPos);
            }
            else if (fog)
            {
                f0 = FogCoefficient(p0.w);
                f1 = FogCoefficient(p1.w);
                f2 = FogCoefficient(p2.w);
            }

            if (texSlot != nullptr)
            {
                const glm::vec2 uv0 = vUV(i0);
                const glm::vec2 uv1 = vUV(i1);
                const glm::vec2 uv2 = vUV(i2);
                // Route masked meshes through our custom packet too (even with fog
                // off): it forces TEX0.TCC=1 so the texel alpha reaches the alpha
                // test. F is 255 here (no fog) and PrimFogEnable is OFF, so no fog is
                // applied. gsKit's own textured prim would use its own TEX0/TCC.
                if (fog || masked)
                {
                    PrimTriTexGouraudFog(&texSlot->mGsTex,
                        x0, y0, iz0, uv0.x * texW, uv0.y * texH, f0,
                        x1, y1, iz1, uv1.x * texW, uv1.y * texH, f1,
                        x2, y2, iz2, uv2.x * texW, uv2.y * texH, f2,
                        c0, c1, c2);
                }
                else
                {
                    gsKit_prim_triangle_goraud_texture_3d(sGsGlobal, &texSlot->mGsTex,
                        x0, y0, iz0, uv0.x * texW, uv0.y * texH,
                        x1, y1, iz1, uv1.x * texW, uv1.y * texH,
                        x2, y2, iz2, uv2.x * texW, uv2.y * texH,
                        c0, c1, c2);
                }
            }
            else if (fog)
            {
                PrimTriGouraudFog(x0, y0, iz0, f0, x1, y1, iz1, f1, x2, y2, iz2, f2,
                                  c0, c1, c2);
            }
            else
            {
                const int izAvg = (iz0 + iz1 + iz2) / 3;
                gsKit_prim_triangle_gouraud(sGsGlobal,
                    x0, y0, x1, y1, x2, y2, izAvg, c0, c1, c2);
            }
        }

        // Leave fog disabled so later non-fog draws (particles, UI) that reuse
        // gsKit's plain XYZ2 prims don't inherit the FGE bit with a stale F.
        sGsGlobal->PrimFogEnable = GS_SETTING_OFF;
        // Restore the persistent src-over blend equation if an additive mesh
        // switched it — the UI font-mask blend (and translucent draws) rely on it.
        if (translucent && blend == BlendMode::Additive)
        {
            gsKit_set_primalpha(sGsGlobal, GS_SETREG_ALPHA(0, 1, 0, 1, 0x80), 0);
        }
        // Restore depth writes after a blended draw.
        if (translucent)
        {
            WriteZWriteMask(0);
        }
        // Restore alpha test OFF (the pipeline default — UI/font AA depends on it).
        if (masked)
        {
            gsKit_set_test(sGsGlobal, GS_ATEST_OFF);
        }
    }
}

void GFX_DrawStaticMeshComp(StaticMesh3D* comp, StaticMesh* meshOverride)
{
    if (sGsGlobal == nullptr || comp == nullptr) return;

    StaticMesh* mesh = (meshOverride != nullptr) ? meshOverride : comp->GetStaticMesh();
    if (mesh == nullptr) return;

    auto it = sStaticMeshes.find(mesh);
    if (it == sStaticMeshes.end()) return;
    const Ps2MeshData& data = it->second;

    // Build MVP from camera + comp world transform.
    World* world = GetWorld(0);
    if (world == nullptr) return;
    Camera3D* camera = world->GetActiveCamera();
    if (camera == nullptr) return;

    const glm::mat4 model = comp->GetRenderTransform();
    const glm::mat4 mvp   = camera->GetViewProjectionMatrix() * model;
    Ps2TextureData* texSlot = GetMeshTexture(comp, mesh);
    SceneLighting light  = GatherLighting(world);
    MaterialLite* mat    = ResolveMaterialLite(comp);
    const bool isSkybox  = comp->As<Skybox3D>() != nullptr;
    const bool wantUnlit = IsCompUnlit(comp) || isSkybox;  // sky = no lighting

    // Vertex-colored meshes store verts in mVerticesColor (mVertices is empty);
    // pass that as the color-vertex source so they draw (and can modulate color).
    const std::vector<VertexColor>* colorVerts = data.mHasColor ? &data.mVerticesColor : nullptr;
    DrawTrisHelper(data.mVertices, data.mIndices, model, mvp, texSlot, light,
                   wantUnlit, isSkybox, mat, colorVerts);
}

// =========================================================================
// Skeletal mesh — Phase 3
// =========================================================================
// Pattern matches PSP (memory: project_psp_skeletal_mesh_cpu_skinning).
// Engine CPU-skins vertices once per frame and hands them to
// GFX_UpdateSkeletalMeshCompVertexBuffer. We stash them in a per-comp vector;
// per-mesh indices come from GFX_CreateSkeletalMeshResource. Draw is then
// identical to static mesh — same DrawTrisHelper.

namespace
{
    std::unordered_map<SkeletalMesh*,     std::vector<IndexType>> sSkeletalMeshIndices;
    std::unordered_map<SkeletalMesh3D*,   std::vector<Vertex>>    sSkeletalCompVerts;
}

void GFX_CreateSkeletalMeshResource(SkeletalMesh* sm,
                                     uint32_t /*numVertices*/, VertexSkinned* /*vertices*/,
                                     uint32_t numIndices, IndexType* indices)
{
    if (sm == nullptr || indices == nullptr) return;
    sSkeletalMeshIndices[sm].assign(indices, indices + numIndices);
    LogDebug("[PS2] CreateSkeletalMeshResource: %u indices", numIndices);
}

void GFX_DestroySkeletalMeshResource(SkeletalMesh* sm)
{
    if (sm == nullptr) return;
    sSkeletalMeshIndices.erase(sm);
}

void GFX_CreateSkeletalMeshCompResource(SkeletalMesh3D* c)
{
    if (c == nullptr) return;
    sSkeletalCompVerts[c];  // ensure slot exists
}

void GFX_DestroySkeletalMeshCompResource(SkeletalMesh3D* c)
{
    if (c == nullptr) return;
    sSkeletalCompVerts.erase(c);
}

void GFX_ReallocateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* c, uint32_t numVerts)
{
    if (c == nullptr) return;
    sSkeletalCompVerts[c].resize(numVerts);
}

void GFX_UpdateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* c, const std::vector<Vertex>& skinnedVertices)
{
    if (c == nullptr) return;
    sSkeletalCompVerts[c] = skinnedVertices;
}

void GFX_DrawSkeletalMeshComp(SkeletalMesh3D* c)
{
    if (sGsGlobal == nullptr || c == nullptr) return;
    SkeletalMesh* mesh = c->GetSkeletalMesh();
    if (mesh == nullptr) return;

    auto itVerts = sSkeletalCompVerts.find(c);
    if (itVerts == sSkeletalCompVerts.end() || itVerts->second.empty()) return;
    auto itIdx = sSkeletalMeshIndices.find(mesh);
    if (itIdx == sSkeletalMeshIndices.end() || itIdx->second.empty()) return;

    World* world = GetWorld(0);
    if (world == nullptr) return;
    Camera3D* camera = world->GetActiveCamera();
    if (camera == nullptr) return;

    const glm::mat4 model = c->GetRenderTransform();
    const glm::mat4 mvp   = camera->GetViewProjectionMatrix() * model;

    Material* matBase = c->GetMaterial();
    MaterialLite* mat = Material::AsLite(matBase ? matBase : Renderer::Get()->GetDefaultMaterial());
    Ps2TextureData* texSlot = nullptr;
    if (mat != nullptr && mat->GetTexture(0) != nullptr)
    {
        auto it = sTextures.find(mat->GetTexture(0));
        if (it != sTextures.end()) texSlot = &it->second;
    }

    SceneLighting light = GatherLighting(world);
    DrawTrisHelper(itVerts->second, itIdx->second, model, mvp, texSlot, light,
                   IsCompUnlit(c), /*invertCull=*/false, mat);
}

bool GFX_IsCpuSkinningRequired(SkeletalMesh3D* /*c*/)
{
    // PS2 has VU1 microcode that could HW-skin, but baseline = engine CPU
    // skinning. Same call as PSP (which also has HW skinning available but
    // hits 8-bone cap). Phase 4+ could lift this to VU1 mode-9 weighting.
    return true;
}

void GFX_DrawShadowMeshComp(ShadowMesh3D* /*c*/) {}

// =========================================================================
// Instanced mesh — Phase 3
// =========================================================================
// PS2 has no HW per-instance attribute path under fixed-function GS, so we
// loop one DrawTrisHelper call per instance with a different model matrix
// (mirrors PSP project_psp_instanced_mesh).

void GFX_DrawInstancedMeshComp(InstancedMesh3D* comp)
{
    if (sGsGlobal == nullptr || comp == nullptr) return;
    StaticMesh* mesh = comp->GetStaticMesh();
    if (mesh == nullptr) return;

    auto itMesh = sStaticMeshes.find(mesh);
    if (itMesh == sStaticMeshes.end()) return;
    const Ps2MeshData& data = itMesh->second;

    const uint32_t numInstances = comp->GetNumInstances();
    if (numInstances == 0) return;

    World* world = GetWorld(0);
    if (world == nullptr) return;
    Camera3D* camera = world->GetActiveCamera();
    if (camera == nullptr) return;

    Material* matBase = comp->GetMaterial();
    MaterialLite* mat = Material::AsLite(matBase ? matBase : Renderer::Get()->GetDefaultMaterial());
    Ps2TextureData* texSlot = nullptr;
    if (mat != nullptr && mat->GetTexture(0) != nullptr)
    {
        auto it = sTextures.find(mat->GetTexture(0));
        if (it != sTextures.end()) texSlot = &it->second;
    }

    SceneLighting light    = GatherLighting(world);
    const glm::mat4 vp     = camera->GetViewProjectionMatrix();
    const glm::mat4 compTr = comp->GetRenderTransform();

    const bool unlit = IsCompUnlit(comp);
    for (uint32_t i = 0; i < numInstances; ++i)
    {
        const glm::mat4 model = compTr * comp->CalculateInstanceTransform((int32_t)i);
        const glm::mat4 mvp   = vp * model;
        DrawTrisHelper(data.mVertices, data.mIndices, model, mvp, texSlot, light,
                       unlit, /*invertCull=*/false, mat);
    }
}

// ----- 3D text mesh -------------------------------------------------------
// ----- TextMesh3D (3D-in-world text geometry) -----------------------------
// Engine extrudes the font glyphs as actual 3D meshes (TextMesh3D, distinct
// from the 2D UI Text widget). Hands us a Vertex array (full pos/normal/uv
// layout). We reuse DrawTrisHelper with a synthesized identity-indices
// list since the verts already come in triangle-list order.

namespace
{
    struct Ps2TextMeshData
    {
        std::vector<Vertex>     mVerts;
        std::vector<IndexType>  mIndices;   // 0,1,2,3,...,N-1
    };
    std::unordered_map<TextMesh3D*, Ps2TextMeshData> sTextMeshes;
}

void GFX_CreateTextMeshCompResource(TextMesh3D* c)
{
    if (c == nullptr) return;
    sTextMeshes[c];
}
void GFX_DestroyTextMeshCompResource(TextMesh3D* c)
{
    if (c == nullptr) return;
    sTextMeshes.erase(c);
}
void GFX_UpdateTextMeshCompVertexBuffer(TextMesh3D* c,
                                         const std::vector<Vertex>& vertices)
{
    if (c == nullptr) return;
    auto& d = sTextMeshes[c];
    d.mVerts = vertices;
    // Sequential indices 0..N-1 — engine emits triangle-list ordered verts.
    const uint32_t n = (uint32_t)vertices.size();
    d.mIndices.resize(n);
    for (uint32_t i = 0; i < n; ++i) d.mIndices[i] = (IndexType)i;
}
void GFX_DrawTextMeshComp(TextMesh3D* c)
{
    if (sGsGlobal == nullptr || c == nullptr) return;
    auto it = sTextMeshes.find(c);
    if (it == sTextMeshes.end() || it->second.mIndices.empty()) return;

    World* world = GetWorld(0);
    if (world == nullptr) return;
    Camera3D* camera = world->GetActiveCamera();
    if (camera == nullptr) return;

    const glm::mat4 model = c->GetRenderTransform();
    const glm::mat4 mvp   = camera->GetViewProjectionMatrix() * model;

    Material* matBase = c->GetMaterial();
    MaterialLite* mat = Material::AsLite(matBase ? matBase : Renderer::Get()->GetDefaultMaterial());
    Ps2TextureData* texSlot = nullptr;
    if (mat != nullptr && mat->GetTexture(0) != nullptr)
    {
        auto texIt = sTextures.find(mat->GetTexture(0));
        if (texIt != sTextures.end()) texSlot = &texIt->second;
    }

    SceneLighting light = GatherLighting(world);
    const bool unlit = (mat != nullptr && mat->GetShadingModel() == ShadingModel::Unlit);
    DrawTrisHelper(it->second.mVerts, it->second.mIndices,
                   model, mvp, texSlot, light,
                   unlit, /*invertCull=*/false, mat);
}

// ----- Voxel / Terrain / TileMap -----------------------------------------
// ----- Voxel3D / Terrain3D / TileMap2D ------------------------------------
// Same shape: engine cooks (VertexColor, IndexType) per comp, we MVP-
// transform and draw via the shared helper. Maps + helpers defined first so
// the per-comp GFX_* functions below (and the TileMap2D block further down)
// can reference them.

namespace
{
    // Storage for any "engine-built vertex+index mesh" node — TileMap2D,
    // Terrain3D, Voxel3D. All three feed VertexColor + IndexType arrays
    // and want the same unlit + alpha-blend draw treatment.
    struct Ps2VertexColorMesh
    {
        std::vector<VertexColor> mVerts;
        std::vector<IndexType>   mIndices;
    };
    std::unordered_map<TileMap2D*, Ps2VertexColorMesh> sTileMaps;
    std::unordered_map<Terrain3D*, Ps2VertexColorMesh> sTerrains;
    std::unordered_map<Voxel3D*,   Ps2VertexColorMesh> sVoxels;

    // Shared draw helper. CPU MVP transform → perspective divide → viewport
    // map → gsKit goraud_texture (textured) or gsKit_prim_triangle_gouraud
    // (untextured) per triangle. No backface cull. The per-vertex BAKED color
    // (VertexColor.mColor) IS the albedo — engine Voxel3D/Terrain3D are Lit +
    // VertexColorMode::Modulate, so we shade `lighting × vertexColor` per vertex
    // (matching desktop); TileMap2D is unlit (self-illuminated 2D art) → color
    // only. Alpha blend ON because tilemap/voxel layers often have transparent
    // edges. Same Z mapping as static meshes so they Z-test against 3D geometry.
    void DrawVertexColorMesh(const Ps2VertexColorMesh& data,
                              const glm::mat4& model,
                              Ps2TextureData* texSlot,
                              const SceneLighting& light,
                              MaterialLite* mat)
    {
        if (sGsGlobal == nullptr || data.mIndices.empty()) return;

        World* world = GetWorld(0);
        if (world == nullptr) return;
        Camera3D* camera = world->GetActiveCamera();
        if (camera == nullptr) return;
        const glm::mat4 mvp = camera->GetViewProjectionMatrix() * model;

        // Material state — unlit (TileMap) skips Lambert; tint/emissive apply as in
        // DrawTrisHelper. Voxel/Terrain are Lit so they now respond to the scene's
        // directional + point lights.
        const bool unlit       = (mat != nullptr && mat->GetShadingModel() == ShadingModel::Unlit);
        const glm::vec3 matTint = (mat != nullptr) ? glm::vec3(mat->GetColor()) : glm::vec3(1.0f);
        const float matEmission = (mat != nullptr) ? mat->GetEmission() : 0.0f;
        const bool needWP       = (light.mNumPoints > 0) && !unlit;

        glm::mat3 normalMat(model);
        if (!(normalMat[0][0] == normalMat[0][0])) normalMat = glm::mat3(1.0f);

        sGsGlobal->PrimAlphaEnable = GS_SETTING_ON;
        const bool fog = sFog.mEnabled;
        sGsGlobal->PrimFogEnable = fog ? GS_SETTING_ON : GS_SETTING_OFF;
        if (fog)
        {
            WriteFogColor(sFog.mColor);
        }
        if (texSlot != nullptr)
        {
            gsKit_TexManager_bind(sGsGlobal, &texSlot->mGsTex);
            gsKit_set_clamp(sGsGlobal, texSlot->mClampMode);
        }
        const float texW = texSlot ? (float)texSlot->mGsTex.Width  : 1.0f;
        const float texH = texSlot ? (float)texSlot->mGsTex.Height : 1.0f;

        // Per-vertex modulation: albedo (baked vertex color) × lighting × tint,
        // plus emissive. Matches DrawTrisHelper's shade() but with vertex color
        // as the albedo instead of a flat material.
        auto shadeVC = [&](const VertexColor& v) -> u64 {
            const glm::vec3 albedo = UnpackVertexColorRGB(v.mColor);
            glm::vec3 lit(1.0f);
            if (!unlit)
            {
                const glm::vec3 nWS = glm::normalize(normalMat * v.mNormal);
                const glm::vec3 wp  = needWP ? glm::vec3(model * glm::vec4(v.mPosition, 1.0f))
                                             : glm::vec3(0.0f);
                lit = ComputeLighting(nWS, wp, light);
            }
            glm::vec3 modc = matTint * lit * albedo + matTint * matEmission;
            return PackModColor(modc, 0x80);   // 0x80 alpha = identity (texel alpha verbatim)
        };

        const uint32_t numTris = (uint32_t)data.mIndices.size() / 3;
        for (uint32_t t = 0; t < numTris; ++t)
        {
            const uint32_t i0 = data.mIndices[t * 3 + 0];
            const uint32_t i1 = data.mIndices[t * 3 + 1];
            const uint32_t i2 = data.mIndices[t * 3 + 2];

            const glm::vec4 p0 = mvp * glm::vec4(data.mVerts[i0].mPosition, 1.0f);
            const glm::vec4 p1 = mvp * glm::vec4(data.mVerts[i1].mPosition, 1.0f);
            const glm::vec4 p2 = mvp * glm::vec4(data.mVerts[i2].mPosition, 1.0f);
            if (p0.w <= 0.0f || p1.w <= 0.0f || p2.w <= 0.0f) continue;

            const float invW0 = 1.0f / p0.w, invW1 = 1.0f / p1.w, invW2 = 1.0f / p2.w;
            const float nx0 = p0.x * invW0, ny0 = p0.y * invW0, nz0 = p0.z * invW0;
            const float nx1 = p1.x * invW1, ny1 = p1.y * invW1, nz1 = p1.z * invW1;
            const float nx2 = p2.x * invW2, ny2 = p2.y * invW2, nz2 = p2.z * invW2;

            const float x0 = (nx0 * 0.5f + 0.5f) * kViewportW;
            const float y0 = (1.0f - (ny0 * 0.5f + 0.5f)) * kViewportH;
            const float x1 = (nx1 * 0.5f + 0.5f) * kViewportW;
            const float y1 = (1.0f - (ny1 * 0.5f + 0.5f)) * kViewportH;
            const float x2 = (nx2 * 0.5f + 0.5f) * kViewportW;
            const float y2 = (1.0f - (ny2 * 0.5f + 0.5f)) * kViewportH;

            const int iz0 = (int)((1.0f - nz0) * 16383.5f);
            const int iz1 = (int)((1.0f - nz1) * 16383.5f);
            const int iz2 = (int)((1.0f - nz2) * 16383.5f);

            const int f0 = fog ? FogCoefficient(p0.w) : 255;
            const int f1 = fog ? FogCoefficient(p1.w) : 255;
            const int f2 = fog ? FogCoefficient(p2.w) : 255;

            const u64 c0 = shadeVC(data.mVerts[i0]);
            const u64 c1 = shadeVC(data.mVerts[i1]);
            const u64 c2 = shadeVC(data.mVerts[i2]);

            if (texSlot != nullptr)
            {
                const glm::vec2& uv0 = data.mVerts[i0].mTexcoord0;
                const glm::vec2& uv1 = data.mVerts[i1].mTexcoord0;
                const glm::vec2& uv2 = data.mVerts[i2].mTexcoord0;
                if (fog)
                {
                    PrimTriTexGouraudFog(&texSlot->mGsTex,
                        x0, y0, iz0, uv0.x * texW, uv0.y * texH, f0,
                        x1, y1, iz1, uv1.x * texW, uv1.y * texH, f1,
                        x2, y2, iz2, uv2.x * texW, uv2.y * texH, f2,
                        c0, c1, c2);
                }
                else
                {
                    gsKit_prim_triangle_goraud_texture_3d(sGsGlobal, &texSlot->mGsTex,
                        x0, y0, iz0, uv0.x * texW, uv0.y * texH,
                        x1, y1, iz1, uv1.x * texW, uv1.y * texH,
                        x2, y2, iz2, uv2.x * texW, uv2.y * texH,
                        c0, c1, c2);
                }
            }
            else if (fog)
            {
                PrimTriGouraudFog(x0, y0, iz0, f0, x1, y1, iz1, f1, x2, y2, iz2, f2,
                                  c0, c1, c2);
            }
            else
            {
                const int izAvg = (iz0 + iz1 + iz2) / 3;
                gsKit_prim_triangle_gouraud(sGsGlobal,
                    x0, y0, x1, y1, x2, y2, izAvg,
                    c0, c1, c2);
            }
        }

        // Leave fog disabled for later non-fog draws (see DrawTrisHelper).
        sGsGlobal->PrimFogEnable = GS_SETTING_OFF;
    }

    // Look up an engine-side texture slot by walking material → texture →
    // sTextures map. Returns nullptr if any link is missing.
    template <typename CompT>
    Ps2TextureData* GetVcMeshTexture(CompT* comp)
    {
        Material* matBase = comp->GetMaterial();
        MaterialLite* mat = Material::AsLite(matBase ? matBase : Renderer::Get()->GetDefaultMaterial());
        if (mat == nullptr || mat->GetTexture(0) == nullptr) return nullptr;
        auto it = sTextures.find(mat->GetTexture(0));
        return (it != sTextures.end()) ? &it->second : nullptr;
    }
}

void GFX_CreateVoxel3DResource(Voxel3D* v)
{
    if (v == nullptr) return;
    sVoxels[v];
}
void GFX_DestroyVoxel3DResource(Voxel3D* v)
{
    if (v == nullptr) return;
    sVoxels.erase(v);
}
void GFX_UpdateVoxel3DResource(Voxel3D* v,
                                const std::vector<VertexColor>& vertices,
                                const std::vector<IndexType>&   indices)
{
    if (v == nullptr) return;
    auto& d = sVoxels[v];
    d.mVerts   = vertices;
    d.mIndices = indices;
}
void GFX_DrawVoxel3D(Voxel3D* v)
{
    if (v == nullptr) return;
    auto it = sVoxels.find(v);
    if (it == sVoxels.end()) return;
    DrawVertexColorMesh(it->second, v->GetRenderTransform(),
                        GetVcMeshTexture(v), GatherLighting(GetWorld(0)),
                        ResolveMaterialLite(v));
}

void GFX_CreateTerrain3DResource(Terrain3D* t)
{
    if (t == nullptr) return;
    sTerrains[t];
}
void GFX_DestroyTerrain3DResource(Terrain3D* t)
{
    if (t == nullptr) return;
    sTerrains.erase(t);
}
void GFX_UpdateTerrain3DResource(Terrain3D* t,
                                  const std::vector<VertexColor>& vertices,
                                  const std::vector<IndexType>&   indices)
{
    if (t == nullptr) return;
    auto& d = sTerrains[t];
    d.mVerts   = vertices;
    d.mIndices = indices;
}
void GFX_DrawTerrain3D(Terrain3D* t)
{
    if (t == nullptr) return;
    auto it = sTerrains.find(t);
    if (it == sTerrains.end()) return;
    DrawVertexColorMesh(it->second, t->GetRenderTransform(),
                        GetVcMeshTexture(t), GatherLighting(GetWorld(0)),
                        ResolveMaterialLite(t));
}

// =========================================================================
// TileMap2D — Phase 3
// =========================================================================
// Engine CPU-builds a triangle mesh from the tilemap's tile grid + tileset
// atlas each time the tilemap changes, then hands us VertexColor + indices.
// We stash per-comp and draw with the same MVP transform path as static
// meshes — unlit (tilemaps are typically self-illuminated 2D art), no
// backface cull (tiles can be viewed from either side at any angle).
// Storage + DrawVertexColorMesh + GetVcMeshTexture live in the namespace
// block above Voxel3D so all three users (Voxel/Terrain/TileMap) see them.

void GFX_CreateTileMap2DResource(TileMap2D* tm)
{
    if (tm == nullptr) return;
    sTileMaps[tm];   // ensure slot exists
}

void GFX_DestroyTileMap2DResource(TileMap2D* tm)
{
    if (tm == nullptr) return;
    sTileMaps.erase(tm);
}

void GFX_UpdateTileMap2DResource(TileMap2D* tm,
                                  const std::vector<VertexColor>& vertices,
                                  const std::vector<IndexType>&   indices)
{
    if (tm == nullptr) return;
    auto& d = sTileMaps[tm];
    d.mVerts   = vertices;
    d.mIndices = indices;
}

void GFX_DrawTileMap2D(TileMap2D* tm)
{
    if (tm == nullptr) return;
    auto it = sTileMaps.find(tm);
    if (it == sTileMaps.end()) return;
    DrawVertexColorMesh(it->second, tm->GetRenderTransform(),
                        GetVcMeshTexture(tm), GatherLighting(GetWorld(0)),
                        ResolveMaterialLite(tm));
}

// ----- Particles ----------------------------------------------------------
// =========================================================================
// Particles — Phase 3
// =========================================================================
// Pattern matches PSP (memory: project_psp_particles).
//
// Engine CPU-simulates particles each frame (Particle3D's tick). When it's
// done, it hands us a flat std::vector<VertexParticle> via
// GFX_UpdateParticleCompVertexBuffer — 4 verts per particle, billboard
// corners pre-oriented to face the camera. We repack 4→6 verts (two
// triangles per quad, indices 0-1-2 / 2-1-3) into a per-comp buffer.
//
// Per-vertex: position (vec3 world or local), UV (vec2), color (u32 RGBA8).
// No normal — particles are unlit. Per-vertex color encodes both tint and
// fade-out alpha (engine sets alpha from the particle's age curve).
//
// useLocalSpace: when true, vertex positions are in the comp's LOCAL frame
// and we apply comp->GetTransform() as the model matrix; when false, they
// already live in world space and we use identity.

namespace
{
    std::unordered_map<Particle3D*, std::vector<VertexParticle>> sParticleVerts;
}

void GFX_CreateParticleCompResource(Particle3D* p)
{
    if (p == nullptr) return;
    sParticleVerts[p];  // ensure slot exists; buffer fills lazily on first update
}

void GFX_DestroyParticleCompResource(Particle3D* p)
{
    if (p == nullptr) return;
    sParticleVerts.erase(p);
}

void GFX_UpdateParticleCompVertexBuffer(Particle3D* p, const std::vector<VertexParticle>& vertices)
{
    if (p == nullptr) return;
    const uint32_t numInputVerts = (uint32_t)vertices.size();
    const uint32_t numParticles  = numInputVerts / 4;
    if (numParticles == 0)
    {
        sParticleVerts[p].clear();
        return;
    }

    // Expand 4 quad-corners → 6 triangle-list verts per particle. Winding
    // 0-1-2 / 2-1-3 matches PSP/GameCube convention (triangle 1 = top-left
    // half, triangle 2 = bottom-right half).
    std::vector<VertexParticle>& out = sParticleVerts[p];
    out.resize(numParticles * 6);
    for (uint32_t i = 0; i < numParticles; ++i)
    {
        const VertexParticle* q = vertices.data() + i * 4;
        VertexParticle*       d = out.data()      + i * 6;
        d[0] = q[0]; d[1] = q[1]; d[2] = q[2];
        d[3] = q[2]; d[4] = q[1]; d[5] = q[3];
    }
}

void GFX_DrawParticleComp(Particle3D* p)
{
    if (sGsGlobal == nullptr || p == nullptr) return;
    if (p->GetNumParticles() == 0) return;

    auto it = sParticleVerts.find(p);
    if (it == sParticleVerts.end() || it->second.empty()) return;
    const std::vector<VertexParticle>& verts = it->second;

    World* world = GetWorld(0);
    if (world == nullptr) return;
    Camera3D* camera = world->GetActiveCamera();
    if (camera == nullptr) return;

    // World-space when useLocalSpace=false (engine has already baked
    // emitter transform into vertex positions); local space when true (we
    // apply the comp's transform here).
    const glm::mat4 model = p->GetUseLocalSpace() ? p->GetTransform()
                                                  : glm::mat4(1.0f);
    const glm::mat4 mvp   = camera->GetViewProjectionMatrix() * model;

    // Texture: from comp's material (or default). Particles modulate
    // texture by per-vertex color (which carries the age-fade alpha).
    Material* matBase = p->GetMaterial();
    MaterialLite* mat = Material::AsLite(matBase ? matBase : Renderer::Get()->GetDefaultMaterial());
    Ps2TextureData* texSlot = nullptr;
    if (mat != nullptr && mat->GetTexture(0) != nullptr)
    {
        auto texIt = sTextures.find(mat->GetTexture(0));
        if (texIt != sTextures.end()) texSlot = &texIt->second;
    }

    // Particles want alpha blending ON — the engine encodes age-fade into
    // each vertex's alpha channel, and overlapping particles should
    // alpha-composite. Restore on entry, the 3D-mesh draw path below
    // disables it for solid meshes.
    sGsGlobal->PrimAlphaEnable = GS_SETTING_ON;

    if (texSlot != nullptr)
    {
        gsKit_TexManager_bind(sGsGlobal, &texSlot->mGsTex);
        gsKit_set_clamp(sGsGlobal, texSlot->mClampMode);
    }
    const float texW = texSlot ? (float)texSlot->mGsTex.Width  : 1.0f;
    const float texH = texSlot ? (float)texSlot->mGsTex.Height : 1.0f;

    const uint32_t numTris = (uint32_t)verts.size() / 3;
    for (uint32_t t = 0; t < numTris; ++t)
    {
        const VertexParticle& v0 = verts[t * 3 + 0];
        const VertexParticle& v1 = verts[t * 3 + 1];
        const VertexParticle& v2 = verts[t * 3 + 2];

        const glm::vec4 p0 = mvp * glm::vec4(v0.mPosition, 1.0f);
        const glm::vec4 p1 = mvp * glm::vec4(v1.mPosition, 1.0f);
        const glm::vec4 p2 = mvp * glm::vec4(v2.mPosition, 1.0f);
        if (p0.w <= 0.0f || p1.w <= 0.0f || p2.w <= 0.0f) continue;

        const float invW0 = 1.0f / p0.w, invW1 = 1.0f / p1.w, invW2 = 1.0f / p2.w;
        const float nx0 = p0.x * invW0, ny0 = p0.y * invW0, nz0 = p0.z * invW0;
        const float nx1 = p1.x * invW1, ny1 = p1.y * invW1, nz1 = p1.z * invW1;
        const float nx2 = p2.x * invW2, ny2 = p2.y * invW2, nz2 = p2.z * invW2;

        const float x0 = (nx0 * 0.5f + 0.5f) * kViewportW;
        const float y0 = (1.0f - (ny0 * 0.5f + 0.5f)) * kViewportH;
        const float x1 = (nx1 * 0.5f + 0.5f) * kViewportW;
        const float y1 = (1.0f - (ny1 * 0.5f + 0.5f)) * kViewportH;
        const float x2 = (nx2 * 0.5f + 0.5f) * kViewportW;
        const float y2 = (1.0f - (ny2 * 0.5f + 0.5f)) * kViewportH;

        // No backface cull for particles — quads are always camera-facing
        // billboards, and the engine emits them with consistent winding.

        const int iz0 = (int)((1.0f - nz0) * 16383.5f);
        const int iz1 = (int)((1.0f - nz1) * 16383.5f);
        const int iz2 = (int)((1.0f - nz2) * 16383.5f);

        // Per-vertex packed RGBA → gsKit RGBAQ. Engine RGBA8 is (r at lsb).
        // No widget-tint multiply for particles; the engine already baked
        // age fade into the alpha channel.
        auto pack = [](uint32_t c) -> u64 {
            const u32 r = ((c >>  0) & 0xFFu);
            const u32 g = ((c >>  8) & 0xFFu);
            const u32 b = ((c >> 16) & 0xFFu);
            const u32 a = ((c >> 24) & 0xFFu);
            // Send engine RGB at FULL 0-255 range, not halved. gsKit's
            // modulate reference is 0x80, so engine_color=0xFF → modulate
            // factor = 255/128 ≈ 2.0 which saturates back to 1.0 — i.e.,
            // a full-bright engine color produces texel*1.0 = unaltered
            // texture. Halving (>> 1) was making 0xFF into 0x7F (mod 0.99x)
            // and 0x80 into 0x40 (mod 0.5x), darkening every particle by
            // ~half. Alpha stays full range so the engine's per-particle
            // age-fade controls the blend.
            return GS_SETREG_RGBAQ(r, g, b, a, 0);
        };
        const u64 c0 = pack(v0.mColor);
        const u64 c1 = pack(v1.mColor);
        const u64 c2 = pack(v2.mColor);

        if (texSlot != nullptr)
        {
            gsKit_prim_triangle_goraud_texture_3d(sGsGlobal, &texSlot->mGsTex,
                x0, y0, iz0, v0.mTexcoord.x * texW, v0.mTexcoord.y * texH,
                x1, y1, iz1, v1.mTexcoord.x * texW, v1.mTexcoord.y * texH,
                x2, y2, iz2, v2.mTexcoord.x * texW, v2.mTexcoord.y * texH,
                c0, c1, c2);
        }
        else
        {
            const int izAvg = (iz0 + iz1 + iz2) / 3;
            gsKit_prim_triangle_gouraud(sGsGlobal,
                x0, y0, x1, y1, x2, y2, izAvg, c0, c1, c2);
        }
    }
}

// ----- UI: Quad / QuadBorder / Text / Poly -------------------------------
// =========================================================================
// UI widgets — Phase 3
// =========================================================================
// Engine widget vertices are VertexUI {vec2 pos, vec2 uv, u32 color}. Quad
// positions are already in screen-pixel coords (origin top-left). Text
// vertices are widget-LOCAL — needs (rect.x + justified.x) offset and
// (scaledTextSize/fontSize) scale applied (mirrors PSP DrawText).
//
// We submit as triangle-list via gsKit_prim_triangle_*_3d directly; positions
// pass through unchanged (no MVP), iz set high so UI draws on top of any
// 3D content under the GEQUAL Z-test convention.
//
// Resource Create/Destroy are no-ops — engine owns the VertexUI buffer.
// We don't need separate VRAM-side storage; per-frame we just walk the
// vertex array.

namespace
{
    constexpr int kUI_Z = 32767;  // largest iz under GEQUAL → UI draws on top

    inline u64 UnpackVertexColor(uint32_t packedRGBA, const glm::vec4& tint)
    {
        // Engine packs as 0xRR_GG_BB_AA in memory (R lowest byte). gsKit
        // modulation uses [0..0x80] = identity. Multiply by tint, scale.
        const float r = ((packedRGBA >>  0) & 0xFFu) * (1.0f / 255.0f) * tint.r;
        const float g = ((packedRGBA >>  8) & 0xFFu) * (1.0f / 255.0f) * tint.g;
        const float b = ((packedRGBA >> 16) & 0xFFu) * (1.0f / 255.0f) * tint.b;
        const float a = ((packedRGBA >> 24) & 0xFFu) * (1.0f / 255.0f) * tint.a;
        return GS_SETREG_RGBAQ(
            (u32)(glm::clamp(r, 0.0f, 1.0f) * 0x80),
            (u32)(glm::clamp(g, 0.0f, 1.0f) * 0x80),
            (u32)(glm::clamp(b, 0.0f, 1.0f) * 0x80),
            (u32)(glm::clamp(a, 0.0f, 1.0f) * 0x80),
            0);
    }

    // Per-widget rotation around a screen-space pivot. Returns (sin, cos)
    // for the rotation matrix. Caller computes:
    //   p' = pivot + Rot(p - pivot)
    // (post-posScale/posOffset, so rotation is in final screen space).
    struct UIRotation
    {
        glm::vec2 mPivot  = glm::vec2(0.0f);
        float     mSin    = 0.0f;
        float     mCos    = 1.0f;
        bool      mActive = false;
    };

    UIRotation MakeUIRotation(Widget* w)
    {
        UIRotation out;
        if (w == nullptr) return out;
        const float degrees = w->GetRotation();
        if (degrees == 0.0f) return out;   // fast path — most widgets don't rotate
        const float rad = degrees * (3.14159265358979323846f / 180.0f);
        const Rect r = w->GetRect();
        const glm::vec2 pn = w->GetPivot();
        out.mPivot  = glm::vec2(r.mX + r.mWidth * pn.x, r.mY + r.mHeight * pn.y);
        out.mSin    = sinf(rad);
        out.mCos    = cosf(rad);
        out.mActive = true;
        return out;
    }

    inline void ApplyRotation(float& x, float& y, const UIRotation& rot)
    {
        if (!rot.mActive) return;
        const float dx = x - rot.mPivot.x;
        const float dy = y - rot.mPivot.y;
        x = rot.mPivot.x + dx * rot.mCos - dy * rot.mSin;
        y = rot.mPivot.y + dx * rot.mSin + dy * rot.mCos;
    }

    void SubmitUITriList(const VertexUI* verts, uint32_t numVerts,
                         const glm::vec4& tint, Ps2TextureData* texSlot,
                         const glm::vec2& posScale, const glm::vec2& posOffset,
                         const UIRotation& rot)
    {
        if (verts == nullptr || numVerts < 3) return;

        if (texSlot != nullptr)
        {
            gsKit_TexManager_bind(sGsGlobal, &texSlot->mGsTex);
            gsKit_set_clamp(sGsGlobal, texSlot->mClampMode);
        }
        const float texW = texSlot ? (float)texSlot->mGsTex.Width  : 1.0f;
        const float texH = texSlot ? (float)texSlot->mGsTex.Height : 1.0f;

        // UI needs alpha blend ON — font glyph masks rely on the
        // src*As+dst*(1-As) equation to render only the glyph shape. 3D
        // mesh draws disable it (above in DrawTrisHelper) for opacity
        // stability; we re-arm it here.
        sGsGlobal->PrimAlphaEnable = GS_SETTING_ON;

        const uint32_t numTris = numVerts / 3;
        for (uint32_t t = 0; t < numTris; ++t)
        {
            const VertexUI& v0 = verts[t * 3 + 0];
            const VertexUI& v1 = verts[t * 3 + 1];
            const VertexUI& v2 = verts[t * 3 + 2];

            float x0 = v0.mPosition.x * posScale.x + posOffset.x;
            float y0 = v0.mPosition.y * posScale.y + posOffset.y;
            float x1 = v1.mPosition.x * posScale.x + posOffset.x;
            float y1 = v1.mPosition.y * posScale.y + posOffset.y;
            float x2 = v2.mPosition.x * posScale.x + posOffset.x;
            float y2 = v2.mPosition.y * posScale.y + posOffset.y;
            // Widget rotation around screen-space pivot (no-op when angle=0).
            ApplyRotation(x0, y0, rot);
            ApplyRotation(x1, y1, rot);
            ApplyRotation(x2, y2, rot);

            const u64 c0 = UnpackVertexColor(v0.mColor, tint);
            const u64 c1 = UnpackVertexColor(v1.mColor, tint);
            const u64 c2 = UnpackVertexColor(v2.mColor, tint);

            if (texSlot != nullptr)
            {
                gsKit_prim_triangle_goraud_texture_3d(sGsGlobal, &texSlot->mGsTex,
                    x0, y0, kUI_Z, v0.mTexcoord.x * texW, v0.mTexcoord.y * texH,
                    x1, y1, kUI_Z, v1.mTexcoord.x * texW, v1.mTexcoord.y * texH,
                    x2, y2, kUI_Z, v2.mTexcoord.x * texW, v2.mTexcoord.y * texH,
                    c0, c1, c2);
            }
            else
            {
                gsKit_prim_triangle_gouraud(sGsGlobal,
                    x0, y0, x1, y1, x2, y2, kUI_Z, c0, c1, c2);
            }
        }
    }

    // Engine widgets emit a triangle-fan winding (Quad: 4 verts; Poly: up
    // to ~16). gsKit doesn't take fan-indexed primitives directly, so we
    // re-emit as triangle list. Static scratch buffer — PS2 widget draw is
    // single-threaded and one draw at a time, so reuse is fine.
    void SubmitUITriFan(const VertexUI* verts, uint32_t numVerts,
                       const glm::vec4& tint, Ps2TextureData* texSlot,
                       const glm::vec2& posScale, const glm::vec2& posOffset,
                       const UIRotation& rot)
    {
        if (verts == nullptr || numVerts < 3) return;
        constexpr uint32_t kMaxFanVerts = 256;
        if (numVerts > kMaxFanVerts) numVerts = kMaxFanVerts;

        static VertexUI sFanBuf[(kMaxFanVerts - 2) * 3];
        const uint32_t numTris = numVerts - 2;
        uint32_t w = 0;
        for (uint32_t t = 0; t < numTris; ++t)
        {
            sFanBuf[w++] = verts[0];
            sFanBuf[w++] = verts[t + 1];
            sFanBuf[w++] = verts[t + 2];
        }
        SubmitUITriList(sFanBuf, numTris * 3, tint, texSlot, posScale, posOffset, rot);
    }

    Ps2TextureData* GetUITexture(Texture* tex)
    {
        if (tex == nullptr) return nullptr;
        auto it = sTextures.find(tex);
        if (it == sTextures.end()) return nullptr;
        return &it->second;
    }
}

// ---- Quad ---------------------------------------------------------------
// Quad widget vertices arrive in screen-pixel coords (engine layout pass
// already applied rect translation). Triangle-fan winding — convert to
// triangle list before submitting.

void GFX_CreateQuadResource(Quad* /*q*/) {}
void GFX_DestroyQuadResource(Quad* /*q*/) {}
void GFX_UpdateQuadResourceVertexData(Quad* /*q*/) {}

void GFX_DrawQuad(Quad* quad)
{
    if (sGsGlobal == nullptr || quad == nullptr) return;
    VertexUI* verts = quad->GetVertices();
    const uint32_t n = quad->GetNumVertices();
    if (verts == nullptr || n < 3) return;
    SubmitUITriFan(verts, n, quad->GetColor(), GetUITexture(quad->GetTexture()),
                   glm::vec2(1.0f, 1.0f), glm::vec2(0.0f, 0.0f),
                   MakeUIRotation(quad));
}

// ---- QuadBorder ---------------------------------------------------------
// Engine API doesn't expose a public GetBorderVertices on PS2's include
// path, and QuadBorder rendering is non-critical for Phase 3. No-op until
// Phase 4 — main UI menus don't strictly need borders to be readable.

void GFX_CreateQuadBorderResource(Quad* /*q*/) {}
void GFX_DestroyQuadBorderResource(Quad* /*q*/) {}
void GFX_UpdateQuadBorderResourceVertexData(Quad* /*q*/) {}
void GFX_DrawQuadBorder(Quad* /*q*/) {}

// ---- Text ---------------------------------------------------------------

void GFX_CreateTextResource(Text* /*t*/) {}
void GFX_DestroyTextResource(Text* /*t*/) {}
void GFX_UpdateTextResourceVertexData(Text* /*t*/) {}

void GFX_DrawText(Text* text)
{
    if (sGsGlobal == nullptr || text == nullptr) return;
    Font* font = text->GetFont();
    if (font == nullptr) return;
    Texture* fontTex = font->GetTexture();
    if (fontTex == nullptr) return;

    const uint32_t numVisible = text->GetNumVisibleCharacters();
    if (numVisible == 0) return;
    const uint32_t numVerts = numVisible * TEXT_VERTS_PER_CHAR;
    VertexUI* verts = text->GetVertices();
    if (verts == nullptr) return;

    // PSP-style: text vertices are widget-LOCAL at font-native size. Apply
    // (rect.x + justified.x, rect.y + justified.y) offset and
    // (scaledTextSize / fontSize) scale so widget anchor + sizing match
    // what Vulkan/GX get via shader uniforms.
    const int32_t fontSize = font->GetSize();
    const float textScale = (fontSize > 0)
        ? (text->GetScaledTextSize() / (float)fontSize) : 1.0f;
    const Rect rect = text->GetRect();
    const glm::vec2 justified = text->GetJustifiedOffset();
    const glm::vec2 posScale(textScale, textScale);
    const glm::vec2 posOffset(rect.mX + justified.x, rect.mY + justified.y);

    SubmitUITriList(verts, numVerts, text->GetColor(), GetUITexture(fontTex),
                    posScale, posOffset, MakeUIRotation(text));
}

// ---- Poly ---------------------------------------------------------------

void GFX_CreatePolyResource(Poly* /*p*/) {}
void GFX_DestroyPolyResource(Poly* /*p*/) {}
void GFX_UpdatePolyResourceVertexData(Poly* /*p*/) {}

void GFX_DrawPoly(Poly* poly)
{
    if (sGsGlobal == nullptr || poly == nullptr) return;
    VertexUI* verts = poly->GetVertices();
    const uint32_t n = poly->GetNumVertices();
    if (verts == nullptr || n < 3) return;
    SubmitUITriFan(verts, n, poly->GetColor(), GetUITexture(poly->GetTexture()),
                   glm::vec2(1.0f, 1.0f), glm::vec2(0.0f, 0.0f),
                   MakeUIRotation(poly));
}

// ----- Direct static mesh draw + post-process ----------------------------
void GFX_DrawStaticMesh(StaticMesh* /*mesh*/, Material* /*material*/,
                         const glm::mat4& /*transform*/, glm::vec4 /*color*/) {}

void GFX_RenderPostProcessPasses() {}

// ----- Matrix helpers ----------------------------------------------------
// Engine consumers (Camera3D) expect right-handed projection matrices.
// Forward to glm directly; identical to what Vulkan/GX paths do under the hood.
glm::mat4 GFX_MakePerspectiveMatrix(float fovyDegrees, float aspectRatio, float zNear, float zFar)
{
    return glm::perspective(glm::radians(fovyDegrees), aspectRatio, zNear, zFar);
}

glm::mat4 GFX_MakeOrthographicMatrix(float left, float right, float bottom, float top, float zNear, float zFar)
{
    return glm::ortho(left, right, bottom, top, zNear, zFar);
}

// ----- Hit-check: scene picking from screen coordinates ------------------
// Phase 0-2 stub. Real impl needs a separate render pass writing instance
// IDs into a 1-px GS framebuffer at (x, y). Out of scope until Phase 3+ adds
// editor-equivalent debug tooling.
Node3D* GFX_ProcessHitCheck(World* /*world*/, int32_t /*x*/, int32_t /*y*/, uint32_t* outInstance)
{
    if (outInstance) *outInstance = 0;
    return nullptr;
}

#endif // POLYPHASE_PLATFORM_ADDON
