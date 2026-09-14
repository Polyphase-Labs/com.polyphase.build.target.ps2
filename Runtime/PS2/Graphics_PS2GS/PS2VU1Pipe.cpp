#include "PS2VU1Pipe.h"

#include "Log.h"
#include "System/System.h"   // SYS_GetTimeMicroseconds

#include <gsKit.h>
#include <dmaKit.h>

// gsKit's dmaInit.h and ps2sdk's dma.h both define the DMA_CHANNEL_* constants,
// to identical values but without guarding the macros, so including both trips
// -Wmacro-redefined. We need dma.h for dma_channel_send_packet2 /
// dma_channel_wait (packet2's transport, which gsKit has no equivalent for), so
// drop gsKit's copies first and let ps2sdk's stand.
#undef DMA_CHANNEL_VIF0
#undef DMA_CHANNEL_VIF1
#undef DMA_CHANNEL_GIF
#undef DMA_CHANNEL_SIF2
#include <dma.h>

#include <packet2.h>
#include <packet2_chain.h>
#include <packet2_vif.h>
#include <packet2_types.h>
#include <kernel.h>

#include <string.h>
#include <math.h>

// ----------------------------------------------------------------------
// The microcode descriptor, defined in assembly at the tail of vu1/im3d.dsm
// (`.global xtcCodeNolight`). Layout must match XTC's xtcMicrocode exactly.
//
// Declared at file scope with C linkage so the symbol name matches the one the
// assembler emitted — an anonymous namespace would be the wrong place for it.
// ----------------------------------------------------------------------
struct XtcMicrocodeSwitch
{
    u32 process;    // VU byte address of the entry point; >>3 before upload
    u32 buf1;       // output buffer 1 (VU qword address)
    u32 buf2;       // output buffer 2
    u32 pad;
};

struct XtcMicrocode
{
    void* code;         // DMAret/MPG chain that uploads the microprogram
    u32   vertexTop;
    u32   vertCount;    // input-buffer capacity in vertices
    u32   numAttribs;   // qwords per vertex (3 for im3d: pos, uv, rgba)
    u32   offset;       // double-buffer stride, = vertCount * numAttribs
    void* desc;
    u32   numVerts[5];  // max verts per batch, indexed by prim type
    XtcMicrocodeSwitch swtch[1];  // [0] = Process (no clip), [1+type] = clip
};

extern "C" XtcMicrocode xtcCodeNolight;

namespace ps2vu1
{
namespace
{
    // ------------------------------------------------------------------
    // VU1 memory map. Mirrors vu1/defines.inc — keep the two in sync.
    // ------------------------------------------------------------------
    enum
    {
        vuMatrix     = 0x3F0,   // matrix0..3 (4 qw), xyzwScale, xyzwOffset,
                                //   clipConsts follow contiguously -> 7 qw
        vuXyzwScale  = 0x3F4,
        vuXyzwOffset = 0x3F5,
        vuClipConsts = 0x3F6,
        vuGifTag     = 0x3FA,   // gifTag, then colorScale -> 2 qw
        vuColorScale = 0x3FB,
        vuCodeSwitch = 0x3FF
    };

    // PACKED-mode GIF register descriptors, in the order default_proc.vu
    // writes them: OUT_STQ, OUT_RGBA, OUT_XYZ.
    enum { kGifRegST = 0x02, kGifRegRGBAQ = 0x01, kGifRegXYZF2 = 0x04 };
    enum { kVertRegs = kGifRegST | (kGifRegRGBAQ << 4) | (kGifRegXYZF2 << 8) };

    // Prim-type indices into numVerts[] / swtch[], matching XTC's xtcPrimType.
    enum { kPrimPoints = 0, kPrimLineList, kPrimLineStrip, kPrimTriList, kPrimTriStrip };

    // ------------------------------------------------------------------
    // Chain buffer sizing. Phase 1 draws a single triangle, but the buffer is
    // sized for a realistic batch so the mesh path can reuse it unchanged:
    // setup is ~16 qw, then numAttribs qwords per vertex.
    // ------------------------------------------------------------------
    enum { kMaxBatchVerts = 96 };
    enum { kChainQwords   = 64 + kMaxBatchVerts * 3 };

    GSGLOBAL*  sGs   = nullptr;
    bool       sReady = false;

    // Two chain buffers so the EE can build batch N+1 while VIF1 is still
    // transferring batch N. Only one DMA can be in flight on channel 1, so the
    // wait happens immediately BEFORE the next send rather than after the last
    // one — the build in between is what overlaps with the transfer.
    packet2_t* sPkt[2] = { nullptr, nullptr };
    int        sPktIdx = 0;

    // Barrier bookkeeping. gsKit's queue and VU1's PATH1 both feed the GS, so
    // they have to be ordered — but only when both actually have work. Tracking
    // gsKit's write pointer lets a run of consecutive VU1 meshes cost nothing
    // at all, instead of a queue_exec and two DMA drains apiece.
    void* sLastPoolCur = nullptr;
    bool  sVifPending  = false;

    // Does gsKit have work that must reach the GS before our next VU1 kick?
    //
    // The first attempt at this inferred the answer from gsKit's own
    // Os_Queue->pool_cur. That was wrong: gsKit also moves that pointer when it
    // flushes and swaps its double-buffered pool, so the check could read
    // "clean" while real work was pending — the barrier was then skipped
    // forever, the queue never drained, and it overflowed its 4 MB pool part
    // way through asset loading.
    //
    // Callers now say so explicitly. Every site that enqueues is one we write
    // ourselves, so this is a fact rather than a deduction.
    bool sGsDirty = true;          // start dirty: the frame's clear is queued

    // Safety valve. If a call site is ever added that enqueues without marking,
    // an explicit flag alone would reintroduce the overflow. Watching how far
    // gsKit's pool pointer has advanced cannot produce a false "clean", only a
    // false "dirty", so it is safe as a backstop even though it was wrong as
    // the primary signal.
    enum { kForceFlushBytes = 192 * 1024 };

    inline bool GsQueueDirty()
    {
        if (sGsDirty) return true;
        // Backstop only — see kForceFlushBytes.
        if (sGs != nullptr && sGs->Os_Queue != nullptr && sLastPoolCur != nullptr)
        {
            const char* cur  = (const char*)sGs->Os_Queue->pool_cur;
            const char* mark = (const char*)sLastPoolCur;
            if (cur > mark && (u32)(cur - mark) >= (u32)kForceFlushBytes) return true;
        }
        return false;
    }
    inline void MarkGsQueueClean()
    {
        sGsDirty = false;
        if (sGs != nullptr && sGs->Os_Queue != nullptr)
            sLastPoolCur = sGs->Os_Queue->pool_cur;
    }

    // VU constant block, uploaded as 7 consecutive qwords at vuMatrix.
    // Laid out to match the VU map exactly: do not reorder.
    struct alignas(16) VuConsts
    {
        float matrix[16];    // column-major, 4 qwords
        float xyzwScale[4];
        float xyzwOffset[4];
        float clipConsts[4];
    };
    VuConsts sConsts;

    // The constant block is uploaded as one UNPACK of this many qwords, so the
    // struct's size and the unpack count must never drift apart.
    enum { kConstQwords = 7 };
    static_assert(sizeof(VuConsts) == kConstQwords * 16,
                  "VuConsts must stay exactly 7 qwords (matrix0..3, xyzwScale, "
                  "xyzwOffset, clipConsts) to match the VU memory map");

    float sNear = 1.0f, sFar = 1000.0f;

    // Screen-Z range. MUST match the EE path in Graphics_PS2GS.cpp, which uses
    //     iz = (int)((1.0f - nz) * 16383.5f)   ->  near = 32767, far = 0
    // Both paths draw into the same Z buffer in the same frame (fog, skybox and
    // translucent meshes still fall back to the EE loop), so a mismatch makes
    // every EE-drawn triangle lose the GEQUAL test against VU1 geometry.
    //
    // GS_PSMZ_16S actually affords 0..65535, so there is a spare bit of depth
    // precision here — but it can only be taken by lifting BOTH paths together.
    float sNearScreen = 32767.0f, sFarScreen = 0.0f;
    bool  sFogEnable = false;
    float sFogStart = 0.0f, sFogEnd = 1.0f;

    // colorScale multiplies the incoming vertex colour in the microcode's
    // preprocessing loop. 1.0 passes 0..255 through untouched, which is what
    // the untextured GS path wants.
    float sColorScale[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    // Vertex staging buffer. Interleaved per vertex, matching im3d's
    // IN_VERTEX/IN_UV/IN_RGBA macros: [pos][uv][rgba], numAttribs qwords apart.
    struct alignas(16) VuVertex
    {
        float pos[4];    // x, y, z, w — w carries the ADC (cull) flag: 0 or 2048
        float uv[4];     // s, t, q, unused
        u32   rgba[4];   // 0..255 each, as integers (the VU itof0's them)
    };
    VuVertex sVerts[kMaxBatchVerts];

    // ---- streaming triangle-list state ---------------------------------
    u32  sBatchPrim  = 0;
    u32  sBatchFill  = 0;   // vertices staged in sVerts
    u32  sBatchLimit = 0;   // vertices per kick, always a multiple of 3
    u32  sTrisDrawn  = 0;
    // Split the mesh path's EE time into real work vs. time BLOCKED waiting on
    // the previous VIF1 transfer. If sVifWaitUs dominates, the EE is not the
    // bottleneck and shaving EE cycles buys nothing.
    u64  sVifWaitUs  = 0;
    u32  sKicks      = 0;
    // Clipping on by default: anything that can cross the near plane needs it,
    // and the plain Process path produces garbage for vertices behind the eye.
    bool sClipping   = true;


    // ------------------------------------------------------------------
    // GIFtag assembly. The microcode overwrites only word 0 (NLOOP + EOP), so
    // everything else — crucially PRE and the full PRIM value — comes from here.
    //
    // gsKit runs with PRMODECONT=1, meaning the GS takes IIP/TME/FGE/ABE/FST
    // from the PRIM field of each GIFtag rather than from the PRMODE register.
    // XTC assumed the opposite (PRMODECONT=0 plus its own cached PRMODE), so we
    // must supply a *complete* PRIM value here, not a bare primitive type.
    // ------------------------------------------------------------------
    inline u64 MakeGifTagLo(u32 nloop, u32 eop, u32 pre, u32 prim, u32 flg, u32 nreg)
    {
        return ((u64)(nloop & 0x7FFF))
             | ((u64)(eop  & 0x1)  << 15)
             | ((u64)(pre  & 0x1)  << 46)
             | ((u64)(prim & 0x7FF) << 47)
             | ((u64)(flg  & 0x3)  << 58)
             | ((u64)(nreg & 0xF)  << 60);
    }

    // Recompute the screen-Z mapping. Ported from XTC's updateZ() (xtc.c:26-56).
    //
    // The VU derives screen z from 1/w rather than from the matrix:
    //     z = zscale/w + zoffset
    // so that near maps HIGH and far maps LOW — which is exactly the polarity
    // the existing GEQUAL Z-test in Graphics_PS2GS.cpp expects.
    void UpdateZ()
    {
        const float n = sNear;
        const float f = sFar;
        float N = sNearScreen;
        float F = sFarScreen;

        // XTC keeps a small guard band at both ends (RenderWare does the same),
        // so geometry exactly on a plane cannot quantise outside the Z range.
        N += (F - N) / 10000.0f;
        F -= (F - N) / 10000.0f;

        sConsts.xyzwScale[2]  = (N - F) * n * f / (f - n);
        sConsts.xyzwOffset[2] = (F * f - N * n) / (f - n);
    }

    void UpdateFog()
    {
        // The VU divides by xyzwScale.w to get 1/w, so it must never be zero
        // even with fog disabled:
        //     q = xyzwScale.w / (w * xyzwScale.w) = 1/w
        // The value cancels out, so 1.0 is a safe no-op scale.
        if (sFogEnable && sFogEnd != sFogStart)
        {
            const float scale = -255.0f / (sFogEnd - sFogStart);
            sConsts.xyzwScale[3]  = scale;
            sConsts.xyzwOffset[3] = -sFogEnd * scale;
            sConsts.clipConsts[0] = sFogStart;
            sConsts.clipConsts[1] = sFogEnd;
        }
        else
        {
            // Clamp window collapses to zero, so F comes out 0 and the ADC bit
            // (which rides in the same word, at 2048.0) stays clear.
            sConsts.xyzwScale[3]  = 1.0f;
            sConsts.xyzwOffset[3] = 0.0f;
            sConsts.clipConsts[0] = 0.0f;
            sConsts.clipConsts[1] = 0.0f;
        }
    }
}   // namespace

// ======================================================================

bool IsReady() { return sReady; }

bool Init(GSGLOBAL* gsGlobal)
{
    if (gsGlobal == nullptr)
    {
        LogError("[PS2/VU1] Init called with null GSGLOBAL");
        return false;
    }
    sGs = gsGlobal;

    // NOTE: VIF1's channel is opened by InitGs() in Graphics_PS2GS.cpp, next to
    // the GIF channel, so that all DMA channel setup lives in one place. Do not
    // dmaKit_chan_init(DMA_CHANNEL_VIF1) here as well — that would reset the
    // channel a second time.

    // Uncached-accelerated: the EE writes the chain through the write buffer
    // rather than the data cache, so the DMAC always sees current bytes.
    // PCSX2 does not emulate the EE data cache, so getting this wrong is
    // invisible in the emulator and fatal on real hardware.
    sPkt[0] = packet2_create(kChainQwords, P2_TYPE_UNCACHED_ACCL, P2_MODE_CHAIN, 1);
    sPkt[1] = packet2_create(kChainQwords, P2_TYPE_UNCACHED_ACCL, P2_MODE_CHAIN, 1);
    if (sPkt[0] == nullptr || sPkt[1] == nullptr)
    {
        LogError("[PS2/VU1] packet2_create failed (%d qwords)", (int)kChainQwords);
        return false;
    }

    memset(&sConsts, 0, sizeof(sConsts));
    // Identity model-view-projection until the caller sets one.
    for (int i = 0; i < 4; ++i) sConsts.matrix[i * 5] = 1.0f;

    SetViewport(0, 0, sGs->Width, sGs->Height);
    UpdateZ();
    UpdateFog();

    sReady = true;

    const XtcMicrocode* mc = &xtcCodeNolight;
    LogDebug("[PS2/VU1] ready: im3d microcode at %p, vertCount=%u numAttribs=%u "
             "offset=%u triListBatch=%u verts",
             mc->code, (unsigned)mc->vertCount, (unsigned)mc->numAttribs,
             (unsigned)mc->offset, (unsigned)mc->numVerts[kPrimTriList]);
    return true;
}

void Shutdown()
{
    for (int i = 0; i < 2; ++i)
    {
        if (sPkt[i] != nullptr) { packet2_free(sPkt[i]); sPkt[i] = nullptr; }
    }
    sGs = nullptr;
    sReady = false;
}

void SetViewport(int x, int y, int width, int height)
{
    if (sGs == nullptr) return;

    sConsts.xyzwScale[0] =  width  * 0.5f;
    sConsts.xyzwScale[1] = -height * 0.5f;   // GS +Y is down, NDC +Y is up

    // The GS primitive coordinate system is a 4096x4096 window with the
    // framebuffer centred in it. gsKit already computed that origin into
    // OffsetX/OffsetY (in 1/16 pixel units), so derive from those rather than
    // hardcoding 2048 — both paths then share one source of truth.
    sConsts.xyzwOffset[0] = sGs->OffsetX / 16.0f + x + width  * 0.5f;
    sConsts.xyzwOffset[1] = sGs->OffsetY / 16.0f + y + height * 0.5f;
}

void SetProjection(const float proj[16])
{
    // Column-major GL convention: a = m[2][2] = proj[10], b = m[3][2] = proj[14].
    const float a = proj[10];
    const float b = proj[14];
    if (a != 1.0f && a != -1.0f)
    {
        sNear = b / (a - 1.0f);
        sFar  = b / (a + 1.0f);
    }
    sConsts.clipConsts[2] = sNear;
    sConsts.clipConsts[3] = sFar;
    UpdateZ();
}

void SetNearFar(float nearZ, float farZ)
{
    if (nearZ <= 0.0f || farZ <= nearZ) return;   // degenerate; keep the last good pair
    sNear = nearZ;
    sFar  = farZ;
    sConsts.clipConsts[2] = sNear;
    sConsts.clipConsts[3] = sFar;
    UpdateZ();
}

void SetModelViewProj(const float mvp[16])
{
    memcpy(sConsts.matrix, mvp, sizeof(sConsts.matrix));
}

void SetFog(bool enable, float fogStart, float fogEnd)
{
    sFogEnable = enable;
    sFogStart  = fogStart;
    sFogEnd    = fogEnd;
    UpdateFog();
}

// ----------------------------------------------------------------------
// Frame segmentation
// ----------------------------------------------------------------------

void BeginSegment()
{
    if (!sReady) return;

    // Nothing queued since the last flush means nothing can be out of order, so
    // skip the barrier entirely. This is the whole point: a scene draws many
    // meshes back to back with no gsKit traffic between them, and paying a
    // queue_exec plus two DMA drains per mesh cost more than the VU1 transform
    // saved — measured at +3.5 ms/frame on hardware.
    if (!GsQueueDirty()) return;

    // Order matters: drain VU1 FIRST. A live PATH1 transfer will preempt the
    // gsKit chain at a packet boundary and splice VU primitives into it.
    if (sVifPending)
    {
        dma_channel_wait(DMA_CHANNEL_VIF1, 0);
        sVifPending = false;
    }

    gsKit_queue_exec(sGs);
    dma_channel_wait(DMA_CHANNEL_GIF, 0);
    MarkGsQueueClean();
}

void EndSegment()
{
    if (!sReady) return;

    // Deliberately does NOT wait. The chain already ends in two VIF FLUSHes, so
    // VIF1 will not report idle until the microprogram and its XGKICK are done;
    // all we need is to remember that a transfer may still be in flight. The
    // wait is deferred to whoever actually needs the GS quiet — the next
    // BeginSegment that finds gsKit work queued, or FlushBeforeGsKit at frame
    // end. A run of VU1 meshes therefore never stalls the EE.
    sVifPending = true;
}

void NoteGsKitWrite() { sGsDirty = true; }

void FlushBeforeGsKit()
{
    if (!sReady) return;
    if (sVifPending)
    {
        dma_channel_wait(DMA_CHANNEL_VIF1, 0);
        sVifPending = false;
    }
}

// ----------------------------------------------------------------------
// Draw
// ----------------------------------------------------------------------
namespace
{
    // Builds and kicks one batch. `numVerts` vertices are taken from sVerts.
    void KickBatch(u32 numVerts, u32 gsPrim)
    {
        const XtcMicrocode* mc = &xtcCodeNolight;
        const u32 stride = mc->numAttribs;          // qwords per vertex

        // swtch[0] = Process, the plain non-clipping path. swtch[1 + primtype]
        // = the clipping entry point (TLClip for triangle lists), which runs
        // full 6-plane homogeneous Sutherland-Hodgman in VU1 and writes to a
        // different pair of output buffers — hence the whole qword, not just
        // the code address, changes with the mode.
        const XtcMicrocodeSwitch* sw = sClipping ? &mc->swtch[1 + kPrimTriList]
                                                 : &mc->swtch[0];

        packet2_t* pkt = sPkt[sPktIdx];
        packet2_reset(pkt, 0);

        // ---- Microcode upload ----------------------------------------
        // mc->code points at a DMAret/MPG chain living in .vutext. A CALL tag
        // runs it (it MPGs the microprogram into VU1 instruction memory) and
        // the DMAret returns here.
        //
        // Phase 1 re-uploads every draw for simplicity. The mesh path will
        // cache the currently-resident microcode and skip this, the way XTC's
        // xtcpSetMicrocode does.
        //
        // The packet is built with TTE (tag transfer enable) on, so a DMA tag
        // occupies only the low 8 bytes of its qword and the high 8 bytes carry
        // two VIF codes. packet2_chain_close_tag() asserts qword alignment, so
        // every tag needs those two slots filled — NOPs where we have nothing
        // useful to say.
        packet2_chain_call(pkt, mc->code, 0, 0, 0);
        packet2_vif_nop(pkt, 0);
        packet2_vif_nop(pkt, 0);
        packet2_chain_close_tag(pkt);

        // ---- Constant + control uploads ------------------------------
        packet2_chain_open_cnt(pkt, 0, 0, 0);
        {
            // Make sure the previous microprogram is done before we overwrite
            // the constants it is reading.
            packet2_vif_flush(pkt, 0);
            packet2_vif_flush(pkt, 0);

            // Input double-buffering: buffer 0 at VU 0, buffer 1 at `offset`.
            // XTOP inside the microcode returns whichever is current.
            packet2_vif_base(pkt, 0, 0);
            packet2_vif_offset(pkt, mc->offset, 0);

            // 7 qwords: matrix0..3, xyzwScale, xyzwOffset, clipConsts.
            //
            // NOTE: packet2_add_data's size is in QWORDS, not bytes — it loops
            // packet2_add_u128. Passing sizeof() here writes 16x too much and
            // desyncs VIF's command stream ("Unknown VifCmd").
            packet2_vif_stcycl(pkt, 4, 4, 0);
            packet2_vif_open_unpack(pkt, P2_UNPACK_V4_32, vuMatrix, 0, 0, 0, 0);
            packet2_add_data(pkt, &sConsts, kConstQwords);
            packet2_vif_close_unpack_manual(pkt, kConstQwords);

            // 2 qwords: the vertex GIFtag, then colorScale.
            packet2_vif_nop(pkt, 0);
            packet2_vif_nop(pkt, 0);
            packet2_vif_stcycl(pkt, 4, 4, 0);
            packet2_vif_open_unpack(pkt, P2_UNPACK_V4_32, vuGifTag, 0, 0, 0, 0);
                // NLOOP is left 0 — the microcode ORs in the real vertex count
                // and the EOP bit before it stores the tag.
                packet2_add_u64(pkt, MakeGifTagLo(0, 1, 1, gsPrim, 0 /*PACKED*/, 3));
                packet2_add_u64(pkt, (u64)kVertRegs);
                packet2_add_data(pkt, sColorScale, 1);   // qwords, not bytes
            packet2_vif_close_unpack_manual(pkt, 2);

            // 1 qword: which routine to run and where to put its output.
            // process is a VU *byte* address; the microcode jumps with `jr`,
            // which indexes 8-byte instruction pairs, hence >>3.
            packet2_vif_nop(pkt, 0);
            packet2_vif_nop(pkt, 0);
            packet2_vif_stcycl(pkt, 4, 4, 0);
            packet2_vif_open_unpack(pkt, P2_UNPACK_V4_32, vuCodeSwitch, 0, 0, 0, 0);
                packet2_add_u32(pkt, sw->process >> 3);
                packet2_add_u32(pkt, sw->buf1);
                packet2_add_u32(pkt, sw->buf2);
                packet2_add_u32(pkt, 0);
            packet2_vif_close_unpack_manual(pkt, 1);
        }
        packet2_chain_close_tag(pkt);

        // ---- Vertices ------------------------------------------------
        // Unpacked as plain V4_32 qwords into the double-buffered input area.
        // Attribute *formats* in the microcode's inputDesc (V4_8 for colour and
        // so on) only apply to the display-list path, which unpacks each
        // attribute array separately; here everything is already qword-aligned.
        packet2_chain_open_cnt(pkt, 0, 0, 0);
        {
            packet2_vif_stcycl(pkt, 4, 4, 0);
            packet2_vif_open_unpack(pkt, P2_UNPACK_V4_32, 0, 1 /*dblBuffered*/, 0, 0, 0);
            packet2_add_data(pkt, sVerts, numVerts * stride);   // qwords
            packet2_vif_close_unpack_manual(pkt, numVerts * stride);
        }
        packet2_chain_close_tag(pkt);

        // ---- Kick ----------------------------------------------------
        packet2_chain_open_cnt(pkt, 0, 0, 0);
        {
            packet2_vif_itop(pkt, numVerts, 0);    // XITOP reads this
            packet2_vif_mscalf(pkt, 0, 0);         // start microprogram at 0
            packet2_vif_nop(pkt, 0);
            packet2_vif_nop(pkt, 0);
            // Stall until the microprogram AND its XGKICK have finished, so
            // EndSegment()'s channel wait is sufficient to guarantee PATH1 idle.
            packet2_vif_flush(pkt, 0);
            packet2_vif_flush(pkt, 0);
        }
        packet2_chain_close_tag(pkt);

        packet2_chain_open_end(pkt, 0, 0);
        packet2_vif_nop(pkt, 0);
        packet2_vif_nop(pkt, 0);
        packet2_chain_close_tag(pkt);

        // Wait for the PREVIOUS batch's transfer, not this one's. Everything
        // above — building this entire chain — ran while that transfer was in
        // flight, which is the overlap the second buffer exists to buy. Waiting
        // after the send instead would idle the EE for the whole transfer.
        if (sVifPending)
        {
            const u64 wT0 = SYS_GetTimeMicroseconds();
            dma_channel_wait(DMA_CHANNEL_VIF1, 0);
            sVifWaitUs += SYS_GetTimeMicroseconds() - wT0;
            sVifPending = false;
        }
        ++sKicks;

        // flush_cache=0: the packet already lives in uncached-accelerated
        // memory, so there are no dirty cache lines to write back.
        dma_channel_send_packet2(pkt, DMA_CHANNEL_VIF1, 0);
        sVifPending = true;
        sPktIdx ^= 1;           // next batch builds into the other buffer
    }

    inline void SetVert(int i, float x, float y, float z,
                        u32 r, u32 g, u32 b, u32 a)
    {
        VuVertex& v = sVerts[i];
        v.pos[0] = x; v.pos[1] = y; v.pos[2] = z;
        v.pos[3] = 0.0f;             // ADC flag clear -> vertex is drawn
        v.uv[0] = 0.0f; v.uv[1] = 0.0f; v.uv[2] = 1.0f; v.uv[3] = 0.0f;
        v.rgba[0] = r; v.rgba[1] = g; v.rgba[2] = b; v.rgba[3] = a;
    }

}

void SetClipping(bool enabled) { sClipping = enabled; }
uint32_t GetTrianglesDrawn()   { return sTrisDrawn; }
uint64_t GetVifWaitUs()        { return sVifWaitUs; }
uint32_t GetKickCount()        { return sKicks; }
void     ResetStats()          { sTrisDrawn = 0; sVifWaitUs = 0; sKicks = 0; }

void BeginTriList(uint32_t gsPrim)
{
    if (!sReady) return;
    const XtcMicrocode* mc = &xtcCodeNolight;

    sBatchPrim = gsPrim;
    sBatchFill = 0;

    // numVerts[] is indexed by prim type and already rounded by the microcode's
    // own arithmetic to a whole number of primitives that fits the input buffer
    // (72 verts = 24 triangles for im3d). Never exceed the staging array.
    sBatchLimit = mc->numVerts[kPrimTriList];
    if (sBatchLimit > kMaxBatchVerts) sBatchLimit = (kMaxBatchVerts / 3) * 3;
}

void PushVertex(float x, float y, float z,
                float u, float v,
                uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    if (!sReady) return;

    VuVertex& vt = sVerts[sBatchFill];
    vt.pos[0] = x; vt.pos[1] = y; vt.pos[2] = z;
    vt.pos[3] = 0.0f;                 // ADC clear -> drawn (Phase 6 sets 2048 to cull)
    vt.uv[0]  = u; vt.uv[1]  = v;
    vt.uv[2]  = 1.0f;                 // Q; the VU replaces it with 1/w
    vt.uv[3]  = 0.0f;
    vt.rgba[0] = r; vt.rgba[1] = g; vt.rgba[2] = b; vt.rgba[3] = a;

    if (++sBatchFill >= sBatchLimit)
    {
        KickBatch(sBatchFill, sBatchPrim);
        sTrisDrawn += sBatchFill / 3;
        sBatchFill = 0;
    }
}

void PushTriListFlat(const FlatVert* flat, const uint32_t* triIdx,
                     uint32_t nTris, const uint32_t* colorsByIndex)
{
    if (!sReady || flat == nullptr || triIdx == nullptr ||
        colorsByIndex == nullptr || nTris == 0)
    {
        return;
    }

    for (uint32_t t = 0; t < nTris; ++t)
    {
        const FlatVert* f = flat + (size_t)triIdx[t] * 3;

        for (int k = 0; k < 3; ++k)
        {
            VuVertex& vt = sVerts[sBatchFill + k];
            vt.pos[0] = f[k].mPos[0];
            vt.pos[1] = f[k].mPos[1];
            vt.pos[2] = f[k].mPos[2];
            vt.pos[3] = 0.0f;             // ADC clear -> drawn
            vt.uv[0]  = f[k].mUV[0];
            vt.uv[1]  = f[k].mUV[1];
            vt.uv[2]  = 1.0f;             // Q; the VU replaces it with 1/w
            vt.uv[3]  = 0.0f;
            // Packed RGBA8, little-endian: r is the low byte. The VU itof0's
            // these, so they go across as four separate integers.
            const u32 c = colorsByIndex[f[k].mIndex];
            vt.rgba[0] =  c        & 0xFF;
            vt.rgba[1] = (c >>  8) & 0xFF;
            vt.rgba[2] = (c >> 16) & 0xFF;
            vt.rgba[3] = (c >> 24) & 0xFF;
        }
        sBatchFill += 3;

        // Same batch boundary as PushVertex: sBatchLimit is always a whole
        // number of triangles, so a triangle never straddles a kick.
        if (sBatchFill >= sBatchLimit)
        {
            KickBatch(sBatchFill, sBatchPrim);
            sTrisDrawn += sBatchFill / 3;
            sBatchFill = 0;
        }
    }
}

void EndTriList()
{
    if (!sReady || sBatchFill == 0) return;
    KickBatch(sBatchFill, sBatchPrim);
    sTrisDrawn += sBatchFill / 3;
    sBatchFill = 0;
}


void DrawTriangleDemo(float offsetX, float offsetY)
{
    if (!sReady) return;

    // Feed the triangle in screen pixels and use an orthographic MVP, so the
    // result can be compared directly against GFX_DrawTriangleDemo's gsKit
    // triangle at the same coordinates. This exercises the whole VU path
    // (unpack -> transform -> divide -> viewport -> XGKICK) while keeping the
    // expected output trivially predictable.
    const float w = (float)sGs->Width;
    const float h = (float)sGs->Height;

    float ortho[16];
    memset(ortho, 0, sizeof(ortho));
    ortho[0]  =  2.0f / w;    // x: [0,w] -> [-1,1]
    ortho[5]  = -2.0f / h;    // y: [0,h] -> [1,-1] (screen Y grows downward)
    ortho[10] =  1.0f;
    ortho[12] = -1.0f;
    ortho[13] =  1.0f;
    ortho[15] =  1.0f;        // w = 1 for every vertex -> divide is a no-op

    SetModelViewProj(ortho);

    // Explicitly non-clipping: this is the Phase 1 reference path, kept exactly
    // as it was when it first drew correctly on hardware, so it stays a known-
    // good baseline to compare the mesh path against.
    const bool prevClip = sClipping;
    sClipping = false;

    SetVert(0, 100.0f + offsetX, 100.0f + offsetY, 0.0f, 0xFF, 0x00, 0x00, 0x80);
    SetVert(1, 300.0f + offsetX, 100.0f + offsetY, 0.0f, 0x00, 0xFF, 0x00, 0x80);
    SetVert(2, 200.0f + offsetX, 300.0f + offsetY, 0.0f, 0x00, 0x00, 0xFF, 0x80);

    // Gouraud triangle, no texture, no fog, alpha blending as gsKit set it up.
    const u32 prim = GS_SETREG_PRIM(GS_PRIM_PRIM_TRIANGLE,
                                    1,  // IIP: gouraud
                                    0,  // TME: no texture
                                    0,  // FGE: no fog
                                    sGs->PrimAlphaEnable,
                                    0,  // AA1
                                    0,  // FST: VU emits ST+Q, not UV
                                    sGs->PrimContext,
                                    0); // FIX
    KickBatch(3, prim);
    sClipping = prevClip;
}

}   // namespace ps2vu1
