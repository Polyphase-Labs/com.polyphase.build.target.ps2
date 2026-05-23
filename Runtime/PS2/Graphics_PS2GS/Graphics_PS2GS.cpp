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
#include <dmaKit.h>
#include <gsToolkit.h>
#include <kernel.h>
#include <malloc.h>     // memalign (newlib's PS2 port)

#include <unordered_map>

#include "PS2GSTypes.h"
#include "PS2GSUtils.h"

namespace
{
    GSGLOBAL* sGsGlobal = nullptr;

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
    gsKit_sync_flip(sGsGlobal);

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
void GFX_SetFog(const FogSettings& /*fogSettings*/) {}
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
}

// ----- Lighting (CPU per-vertex Lambert) ----------------------------------
// PS2 GS has no built-in lighting. We bake one directional light's
// contribution into per-vertex modulation color CPU-side. Walks the world
// for the first DirectionalLight3D; if none, falls back to ambient white.
// Engine vertex normals are in model space; we transform by model[0..2][0..2]
// (no inverse-transpose — non-uniform scale will skew, acceptable for v1).
namespace
{
    struct PointLightish
    {
        glm::vec3 mDir   = glm::vec3(0.0f, -1.0f, 0.0f); // world-space direction
        glm::vec3 mColor = glm::vec3(1.0f, 1.0f, 1.0f);
        bool      mFound = false;
    };

    PointLightish GatherMainLight(World* world)
    {
        PointLightish out;
        if (world == nullptr) return out;
        DirectionalLight3D* light = world->FindNode<DirectionalLight3D>();
        if (light == nullptr) return out;
        glm::vec3 dir = light->GetDirection();
        if (glm::length(dir) > 0.0001f) out.mDir = glm::normalize(dir);
        const glm::vec4 col = light->GetColor();
        out.mColor = glm::vec3(col.r, col.g, col.b);
        out.mFound = true;
        return out;
    }

    // Compute the modulation color for one vertex: clamp(N·L, 0..1) * lightCol
    // + ambient. Alpha 0x80 = gsKit identity = "use texel alpha verbatim";
    // anything less attenuates source alpha. (Was 0x00 in earlier passes
    // when no alpha blend was wired up — became "fully transparent" once we
    // enabled real src-over blending for font glyph mask support.)
    inline u64 ShadeVertex(const glm::vec3& normalWS, const PointLightish& light)
    {
        constexpr float kAmbient = 0.25f;
        float dotNL = light.mFound ? -glm::dot(normalWS, light.mDir) : 1.0f;
        // NaN/Inf trap: if any vertex normal or light dir snuck through as
        // garbage, dot produces NaN, then clamp + multiply propagate it,
        // then (u32)(NaN * 0x80) is undefined per C++ spec and typically
        // resolves to 0 (cube renders black) or 0xFFFFFFFF (full bright).
        // Either looks like a single-frame lighting glitch on an animated
        // mesh. Detect + neutralize before it reaches the cast.
        if (!(dotNL == dotNL))   // true when NaN
        {
            dotNL = 1.0f;
        }
        const float n_dot_l = glm::clamp(dotNL, 0.0f, 1.0f);
        glm::vec3 col = light.mColor * (kAmbient + (1.0f - kAmbient) * n_dot_l);
        col = glm::clamp(col, glm::vec3(0.0f), glm::vec3(1.0f));
        const u32 r = (u32)(col.r * 0x80);
        const u32 g = (u32)(col.g * 0x80);
        const u32 b = (u32)(col.b * 0x80);
        // Alpha 0xFF (overbright) instead of 0x80 (identity). The GS blend
        // equation `(Cs-Cd)*As+Cd` interprets source alpha via /128, so
        // 0xFF → As=255/128≈2.0 saturated to 1.0 → guaranteed opaque
        // output. With 0x80 modulation, a texel with alpha < 255 (mipmap
        // artifact, PNG decoder quirk, padding band edge) produced partial
        // blend with the framebuffer — that's what generated the
        // cube's "lighting flicker" symptom (darker with sky behind,
        // half-bright with clear color behind). Forcing alpha overbright
        // makes 3D mesh draws blend-immune regardless of texel alpha.
        return GS_SETREG_RGBAQ(r, g, b, 0xFF, 0);
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
                        const PointLightish& light,
                        bool unlit,
                        bool invertCull)
    {
        if (sGsGlobal == nullptr || verts.empty() || indices.empty()) return;

        // 3D meshes always render opaque — disable PRIM's ABE bit for the
        // duration of this draw. gsKit reads gsGlobal->PrimAlphaEnable when
        // emitting the PRIM register for each prim call, so toggling here
        // sets every subsequent prim to ABE=0. The cube's intermittent
        // "lighting flicker" symptom (darker with sky, half-bright without)
        // was the cube alpha-blending with the framebuffer on some frames
        // even though my computed RGBA was provably stable — the GS was
        // re-blending the modulated output with dst in some way our
        // stable-input math couldn't influence. Skipping blend at the PRIM
        // level eliminates that variable entirely. UI draws re-enable
        // PrimAlphaEnable below for the font glyph mask blend they need.
        sGsGlobal->PrimAlphaEnable = GS_SETTING_OFF;

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

            const glm::vec4 p0 = mvp * glm::vec4(verts[i0].mPosition, 1.0f);
            const glm::vec4 p1 = mvp * glm::vec4(verts[i1].mPosition, 1.0f);
            const glm::vec4 p2 = mvp * glm::vec4(verts[i2].mPosition, 1.0f);
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
            const float signedArea = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            constexpr float kCullEpsilon = 0.5f;
            if (invertCull
                ? (signedArea <=  kCullEpsilon)
                : (signedArea >= -kCullEpsilon))
            {
                continue;
            }

            const int iz0 = (int)((1.0f - nz0) * 16383.5f);
            const int iz1 = (int)((1.0f - nz1) * 16383.5f);
            const int iz2 = (int)((1.0f - nz2) * 16383.5f);

            u64 c0, c1, c2;
            if (unlit)
            {
                // No Lambert — texture displays at full intensity.
                c0 = c1 = c2 = UnlitModulationColor();
            }
            else
            {
                // NaN-safe normalize: when the model matrix has a
                // degenerate row at animation extremes (e.g. HeroSpinner
                // sin-wave touching 0 scale on an axis), normalMat × normal
                // can collapse to (0,0,0). glm::normalize divides by 0 →
                // NaN, which through ShadeVertex produces a junk RGBAQ that
                // the GS interprets as random brightness for that single
                // frame. Fall back to a known-good "up" normal in that case
                // — visually indistinguishable from a proper normal on a
                // single-frame transient.
                auto safeNormalize = [&](const glm::vec3& v) -> glm::vec3 {
                    const float len2 = glm::dot(v, v);
                    return (len2 > 1e-12f) ? (v / glm::sqrt(len2))
                                           : glm::vec3(0.0f, 1.0f, 0.0f);
                };
                const glm::vec3 n0 = safeNormalize(normalMat * verts[i0].mNormal);
                const glm::vec3 n1 = safeNormalize(normalMat * verts[i1].mNormal);
                const glm::vec3 n2 = safeNormalize(normalMat * verts[i2].mNormal);
                c0 = ShadeVertex(n0, light);
                c1 = ShadeVertex(n1, light);
                c2 = ShadeVertex(n2, light);
            }

            if (texSlot != nullptr)
            {
                const glm::vec2& uv0 = verts[i0].mTexcoord0;
                const glm::vec2& uv1 = verts[i1].mTexcoord0;
                const glm::vec2& uv2 = verts[i2].mTexcoord0;
                gsKit_prim_triangle_goraud_texture_3d(sGsGlobal, &texSlot->mGsTex,
                    x0, y0, iz0, uv0.x * texW, uv0.y * texH,
                    x1, y1, iz1, uv1.x * texW, uv1.y * texH,
                    x2, y2, iz2, uv2.x * texW, uv2.y * texH,
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
    PointLightish light = GatherMainLight(world);
    const bool isSkybox  = comp->As<Skybox3D>() != nullptr;
    const bool wantUnlit = IsCompUnlit(comp) || isSkybox;  // sky = no lighting

    DrawTrisHelper(data.mVertices, data.mIndices, model, mvp, texSlot, light, wantUnlit, isSkybox);
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

    PointLightish light = GatherMainLight(world);
    DrawTrisHelper(itVerts->second, itIdx->second, model, mvp, texSlot, light, IsCompUnlit(c), /*invertCull=*/false);
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

    PointLightish light    = GatherMainLight(world);
    const glm::mat4 vp     = camera->GetViewProjectionMatrix();
    const glm::mat4 compTr = comp->GetRenderTransform();

    const bool unlit = IsCompUnlit(comp);
    for (uint32_t i = 0; i < numInstances; ++i)
    {
        const glm::mat4 model = compTr * comp->CalculateInstanceTransform((int32_t)i);
        const glm::mat4 mvp   = vp * model;
        DrawTrisHelper(data.mVertices, data.mIndices, model, mvp, texSlot, light, unlit, /*invertCull=*/false);
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

    PointLightish light = GatherMainLight(world);
    DrawTrisHelper(it->second.mVerts, it->second.mIndices,
                   model, mvp, texSlot, light,
                   /*unlit=*/false, /*invertCull=*/false);
}

// ----- Voxel / Terrain / TileMap -----------------------------------------
// ----- Voxel3D / Terrain3D ------------------------------------------------
// Same shape as TileMap2D: engine cooks (VertexColor, IndexType) per comp,
// we MVP-transform and draw via the shared helper.

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
                        GetVcMeshTexture(v));
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
                        GetVcMeshTexture(t));
}

// =========================================================================
// TileMap2D — Phase 3
// =========================================================================
// Engine CPU-builds a triangle mesh from the tilemap's tile grid + tileset
// atlas each time the tilemap changes, then hands us VertexColor + indices.
// We stash per-comp and draw with the same MVP transform path as static
// meshes — unlit (tilemaps are typically self-illuminated 2D art), no
// backface cull (tiles can be viewed from either side at any angle).

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
    // (untextured) per triangle. No backface cull, no lighting modulation —
    // VertexColor already has per-vertex color baked in (engine
    // pre-shades). Alpha blend ON because tilemap/voxel layers often have
    // transparent edges. Same Z mapping as static meshes so they Z-test
    // correctly against lit 3D geometry.
    void DrawVertexColorMesh(const Ps2VertexColorMesh& data,
                              const glm::mat4& model,
                              Ps2TextureData* texSlot)
    {
        if (sGsGlobal == nullptr || data.mIndices.empty()) return;

        World* world = GetWorld(0);
        if (world == nullptr) return;
        Camera3D* camera = world->GetActiveCamera();
        if (camera == nullptr) return;
        const glm::mat4 mvp = camera->GetViewProjectionMatrix() * model;

        sGsGlobal->PrimAlphaEnable = GS_SETTING_ON;
        if (texSlot != nullptr)
        {
            gsKit_TexManager_bind(sGsGlobal, &texSlot->mGsTex);
            gsKit_set_clamp(sGsGlobal, texSlot->mClampMode);
        }
        const float texW = texSlot ? (float)texSlot->mGsTex.Width  : 1.0f;
        const float texH = texSlot ? (float)texSlot->mGsTex.Height : 1.0f;
        const u64 kIdentity = GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0);

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

            if (texSlot != nullptr)
            {
                const glm::vec2& uv0 = data.mVerts[i0].mTexcoord0;
                const glm::vec2& uv1 = data.mVerts[i1].mTexcoord0;
                const glm::vec2& uv2 = data.mVerts[i2].mTexcoord0;
                gsKit_prim_triangle_goraud_texture_3d(sGsGlobal, &texSlot->mGsTex,
                    x0, y0, iz0, uv0.x * texW, uv0.y * texH,
                    x1, y1, iz1, uv1.x * texW, uv1.y * texH,
                    x2, y2, iz2, uv2.x * texW, uv2.y * texH,
                    kIdentity, kIdentity, kIdentity);
            }
            else
            {
                const int izAvg = (iz0 + iz1 + iz2) / 3;
                gsKit_prim_triangle_gouraud(sGsGlobal,
                    x0, y0, x1, y1, x2, y2, izAvg,
                    kIdentity, kIdentity, kIdentity);
            }
        }
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
                        GetVcMeshTexture(tm));
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
