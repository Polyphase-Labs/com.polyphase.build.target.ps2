#pragma once

// ============================================================================
// PS2VU1Pipe — VU1 geometry pipeline for the PS2 GS backend.
//
// The gsKit path in Graphics_PS2GS.cpp transforms every vertex on the EE in
// scalar float math and emits one GIF packet per triangle. This module moves
// transform / perspective-divide / clipping onto VU1 instead, using the
// microcode vendored in vu1/ from XTC (https://github.com/aap/xtc, MIT).
//
// DIVISION OF LABOUR — gsKit keeps owning everything except geometry:
//   gsKit : video mode, framebuffer + Z allocation, VRAM, texture upload and
//           TexManager, clears, vsync/flip, UI/2D, and all GS *render state*
//           registers (TEX0, ALPHA, TEST, ZBUF, FRAME, SCISSOR).
//   VU1   : vertex transform, perspective divide, clipping, fog coefficient,
//           and emitting the primitive's GIF packet via XGKICK.
//
// Lighting deliberately stays on the EE. The `im3d` (nolight) microprogram
// takes position + ST + RGBA and passes the colour through untouched, which is
// exactly the shape Graphics_PS2GS.cpp's ComputeLighting() already produces —
// so ambient, directional, point and spot lights all keep working unchanged.
//
// ORDERING — this is the part that bites. gsKit's queue is *deferred*: it is a
// chain kicked once at gsKit_queue_exec. VU1 XGKICKs happen when the chain is
// sent. Left unsynchronised, VU1 primitives reach the GS before gsKit_clear
// and get wiped. Callers must bracket VU1 work with BeginSegment()/EndSegment(),
// which drain the two DMA channels so the two streams stay in program order.
// ============================================================================

#include <gsKit.h>
#include <stdint.h>   // gsKit brings in ps2sdk's u32 but not the C99 names

namespace ps2vu1
{
    // Uploads the microcode-independent state and allocates the DMA chain
    // buffer. Must be called after gsKit_init_screen (it reads OffsetX/OffsetY
    // and Width/Height off the GSGLOBAL).
    bool Init(GSGLOBAL* gsGlobal);
    void Shutdown();

    bool IsReady();

    // ---- Camera / viewport state -------------------------------------------

    // `proj` is a column-major GL-convention projection matrix (what
    // glm::perspective produces). Near/far are recovered from it the way XTC
    // does, and feed both the depth mapping and the VU clip constants.
    void SetProjection(const float proj[16]);

    // Equivalent to SetProjection when the caller already knows the camera's
    // clip planes (Camera3D::GetNearZ/GetFarZ) — avoids re-deriving them from
    // the projection matrix. Feeds the screen-Z mapping and the VU clip
    // constants, so it must be current before any BeginTriList.
    void SetNearFar(float nearZ, float farZ);

    // Combined projection * view * model, column-major. glm::mat4 is already
    // column-major, so `SetModelViewProj(&mvp[0][0])` is a straight copy.
    void SetModelViewProj(const float mvp[16]);

    // Defaults to the full framebuffer at Init.
    void SetViewport(int x, int y, int width, int height);

    // Linear fog matching the GS FOG register. Pass enable=false to disable;
    // the VU still computes an F value but the GS ignores it when FGE=0.
    void SetFog(bool enable, float fogStart, float fogEnd);

    // ---- Frame segmentation ------------------------------------------------

    // Flush gsKit's queue and wait for the GIF channel to drain, so everything
    // gsKit has recorded so far lands on the GS *before* any VU1 primitive.
    void BeginSegment();

    // Marks the VU1 work as submitted. Deliberately does NOT stall: the wait is
    // deferred to whoever actually needs the GS quiet, so a run of consecutive
    // VU1 meshes costs no barriers at all.
    void EndSegment();

    // Drain any in-flight VU1 transfer. Must be called before gsKit is allowed
    // to send its own chain — a live PATH1 will preempt a gsKit chain at a
    // packet boundary and splice VU1 primitives into it. BeginSegment does this
    // automatically when it finds gsKit work queued; call this directly at
    // frame end, where gsKit_queue_exec happens outside any segment.
    void FlushBeforeGsKit();

    // Tell the pipe that something was just put into gsKit's draw queue, so the
    // next BeginSegment knows it has to flush before kicking VU1.
    //
    // Call this at EVERY site that enqueues: gsKit_clear, gsKit_prim_*,
    // gsKit_set_* , gsKit_TexManager_bind, and any hand-built packet written
    // through gsKit_heap_alloc. Missing one risks a VU1 primitive overtaking
    // state it depends on; there is a size-based backstop, but it is a backstop
    // and not a substitute.
    void NoteGsKitWrite();

    // ---- Triangle-list submission ------------------------------------------
    //
    // Streaming API: the caller pushes de-indexed triangle vertices and batches
    // are kicked automatically whenever the VU1 input buffer fills (72 verts =
    // 24 triangles for the im3d microprogram). This keeps the EE-side vertex
    // buffer to one batch regardless of mesh size.
    //
    // Colour is passed as four 0..255 integers, already lit and packed through
    // the GS overbright range by the caller — VU1 does no lighting.
    // Texture coordinates are NORMALISED (0..1); the VU emits ST+Q, so the GS
    // scales them by the TEX0 dimensions and interpolation is
    // perspective-correct (unlike the EE path's FST=1 / UV route).
    //
    // GS render state — TEX0, ALPHA, TEST, ZBUF — must already be on the GS
    // before BeginTriList: set it through gsKit as usual, then BeginSegment()
    // flushes it ahead of the VU1 kick.
    // A de-indexed vertex: three per triangle, built once per static mesh by
    // the renderer and reused every frame (positions and UVs are object-space
    // constants). mIndex is the ORIGINAL vertex index, kept so per-vertex
    // colour -- which does change per frame -- can still be looked up.
    struct FlatVert
    {
        float    mPos[3];
        float    mUV[2];
        uint32_t mIndex;
    };

    void BeginTriList(uint32_t gsPrim);
    void PushVertex(float x, float y, float z,
                    float u, float v,
                    uint32_t r, uint32_t g, uint32_t b, uint32_t a);
    // Bulk form of the PushVertex loop for an already-de-indexed, already-
    // culled mesh. triIdx/nTris select the surviving triangles; colorsByIndex
    // is packed RGBA8 indexed by FlatVert::mIndex.
    //
    // Worth having as its own entry point: the per-vertex form was ~3400
    // cross-TU calls per frame, each marshalling nine arguments. Here the loop
    // lives next to the staging buffer and the compiler can see all of it.
    // Call between BeginTriList and EndTriList exactly like PushVertex.
    void PushTriListFlat(const FlatVert* flat, const uint32_t* triIdx,
                         uint32_t nTris, const uint32_t* colorsByIndex);

    void EndTriList();

    // Select the microcode entry point. Clipping on = full 6-plane homogeneous
    // Sutherland-Hodgman in VU1 (required for anything that can cross the near
    // plane); off = the plain Process path, valid only when geometry is known
    // to be fully in front of the camera.
    void SetClipping(bool enabled);

    // Triangles kicked since the last ResetStats(), for A/B measurement.
    uint32_t GetTrianglesDrawn();
    // EE microseconds spent blocked on the previous VIF1 transfer, and the
    // number of batches kicked, since the last ResetStats().
    uint64_t GetVifWaitUs();
    uint32_t GetKickCount();
    void     ResetStats();

    // ---- Phase 1 proof-of-life ---------------------------------------------

    // Draws one gouraud triangle through the full VU1 path. Coordinates are in
    // the same screen-pixel space as GFX_DrawTriangleDemo so the two can be
    // compared side by side.
    void DrawTriangleDemo(float offsetX, float offsetY);
}
