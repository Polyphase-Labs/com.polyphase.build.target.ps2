/**
 * @file Main_PS2.cpp
 * @brief PS2 EE-side entry point + Oct lifecycle hooks for Polyphase.
 *
 * Phase 1 minimum:
 *   - SifInitRpc + IOP reset + sbv_patches to prepare the EE/IOP bridge
 *     (required when running via PCSX2 `-elf` boot, otherwise subsequent
 *     SifLoadFileInit/fileXioInit calls hang forever).
 *   - init_scr() so scr_printf works for boot-phase tty checkpoints.
 *   - OctPreInitialize / OctPostInitialize force 640x448 (NTSC) window size.
 *     Both are needed because ReadEngineConfig (between the two) clobbers
 *     OctPreInitialize values from Config.ini. PostPackage rewrites the
 *     packaged Config.ini to match, so this is belt-and-suspenders.
 *   - Wires gEmbeddedScripts into EngineConfig so Lua components run.
 *
 * Phase 3 will add: IRX module loads (libpad / audsrv / fileXio), audio +
 * input bring-up, real region-aware video mode selection.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <iopheap.h>        // SifInitIopHeap — required before SifLoadModuleBuffer
#include <sbv_patches.h>
#include <debug.h>           // init_scr / scr_printf — boot-phase tty
#include <audsrv.h>          // EE-side stubs for the audsrv IOP module
#include <libmc.h>           // EE-side stubs for mcman/mcserv — memory card
// fileXioInit declared by hand: including <fileXio_rpc.h> trips ps2sdk newlib's
// "Using fio/fileXio functions directly ... will lead to problems" #error. We are
// not doing file I/O through fileXio - we only need to bring the RPC link up so
// that iomanX-backed devices (mmce0:) are reachable at all.
extern "C" int fileXioInit(void);
#include <stdio.h>
#include <string.h>

// Embedded audsrv.irx — generated at build time by `$(PS2SDK)/bin/bin2c`
// (see Makefile_PS2). bin2c emits a C source file declaring
//   unsigned char audsrv_irx[]      = { 0x7f, 0x45, 0x4c, 0x46, ... };
//   unsigned int  size_audsrv_irx   = sizeof(audsrv_irx);
// We pass the buffer to SifExecModuleBuffer so the IOP loads audsrv from
// EE RAM (works under PCSX2 -elf and from disc/MC alike).
extern "C" unsigned char audsrv_irx[];
extern "C" unsigned int  size_audsrv_irx;

// Embedded MMCE drivers (SD2PSX / MemCard PRO). mmceman registers mmce0:,
// which is the device's real SD filesystem - NOT the 8 MB emulated memory
// card at mc0:. mmcedrv is the faster bulk-transfer path and is optional.
extern "C" unsigned char iomanx_irx[];
extern "C" unsigned int  size_iomanx_irx;
extern "C" unsigned char filexio_irx[];
extern "C" unsigned int  size_filexio_irx;
extern "C" unsigned char mmceman_irx[];
extern "C" unsigned int  size_mmceman_irx;
extern "C" unsigned char mmcedrv_irx[];
extern "C" unsigned int  size_mmcedrv_irx;

#include "Engine.h"
#include "Engine/Renderer.h"   // Renderer::EnableConsole
extern void Ps2_LogHeap(const char* tag);   // System_PS2.cpp
#include "Engine/Profiler.h"   // stall attribution
#include <algorithm>
#include "EmbeddedFile.h"
#include "Log.h"

extern uint32_t gNumEmbeddedScripts;
extern EmbeddedFile gEmbeddedScripts[];

extern void GameMain(int32_t argc, char** argv);

namespace
{
    // Append a boot-phase log line to the on-screen tty. Don't fopen the
    // log file here — System_PS2's Ps2_AppendLogLineRaw owns the FILE*
    // (opened once with line-buffering). Hot-opening per call burns
    // milliseconds on PCSX2's host: backend; keep boot-tty path light.
    void Ps2_BootLog(const char* msg)
    {
        scr_printf("%s\n", msg);
    }
}

void OctPreInitialize(EngineConfig& config)
{
    GetEngineState()->mStandalone = true;

    // PS2 NTSC mode: 640x448 visible. PAL is 640x512 — Phase 3 will plumb
    // region selection through. For Phase 0-2 we hardcode NTSC. Memory:
    // project_psp_force_window_size — EngineConfig defaults are 1280x720,
    // so we MUST set unconditionally (NOT gated on `== 0`).
    config.mWindowWidth  = 640;
    config.mWindowHeight = 448;

    // Engine.cpp:1740-1748 force UseAssetRegistry=true on Android/GameCube/
    // Wii/N3DS, but PS2 isn't in that list (because PS2 is an addon-platform
    // and basePlatform=Linux). Without the asset registry, the engine
    // falls back to directory iteration via SYS_OpenDirectory — which we
    // stub for Phase 0-2 — and finds zero assets. Force-enable here.
    // PostPackage also writes UseAssetRegistry=1 to the packaged Config.ini
    // so ReadEngineConfig (which fires AFTER this hook) won't clobber.
    config.mUseAssetRegistry = true;

    // Embed Lua scripts but NOT assets (PS2 has 32 MB main RAM, same budget
    // as PSP; cooked-asset blob would exceed the budget). Memory:
    // project_psp_scripts_embedded_assets_disk.
    config.mEmbeddedScriptCount = gNumEmbeddedScripts;
    config.mEmbeddedScripts     = gEmbeddedScripts;

    Ps2_BootLog("[PS2] OctPreInitialize: window=640x448 NTSC, UseAssetRegistry=1, embeddedScripts wired");
}

void OctPostInitialize()
{
    // ReadEngineConfig fired between Pre and Post — clobbers the resolution
    // we set above with whatever Config.ini holds. Override again so
    // EngineState (which Renderer reads live every frame) matches PS2 native.
    GetEngineState()->mWindowWidth  = 640;
    GetEngineState()->mWindowHeight = 448;
    LogDebug("[PS2] OctPostInitialize: forced viewport 640x448 (NTSC)");
    Ps2_LogHeap("after engine init");

    // Hide the on-screen log console. Renderer::Initialize only hides it for
    //   (PLATFORM_WINDOWS || PLATFORM_LINUX || PLATFORM_MAC || PLATFORM_ANDROID) && !_DEBUG
    // and PS2 is in neither list, so it was left VISIBLE and drawing every log
    // line the game emits.
    //
    // Measured on hardware: four ConsoleOutputText widgets at ~127 glyphs each,
    // 4.7 ms/frame - more than every 3D triangle in the scene combined, and
    // larger than the whole tilemap. It is also self-inflating: the per-second
    // profiler line is itself one of the messages being rendered.
    //
    // Re-enable from Lua with Renderer.EnableConsole(true) when you want to read
    // logs on a console with no ps2link attached.
    if (Renderer::Get() != nullptr)
    {
        Renderer::Get()->EnableConsole(false);
        LogDebug("[PS2] on-screen console disabled (Renderer.EnableConsole(true) re-enables)");
    }
}

// NOTE: the VU1 bring-up test is NOT wired in here. OctPreUpdate/OctPostUpdate
// both sit OUTSIDE the render frame (Engine.cpp:2294-2296 calls them either side
// of Update(), which contains the whole clear->draw->flip sequence), so drawing
// from either hook lands in a buffer that is about to be cleared or flipped
// away. The test lives in GFX_EndFrame instead — see PS2_VU1_BRINGUP_DEMO in
// Graphics_PS2GS.cpp.
void OctPreUpdate()    {}
// Stall hunter, take three.
//
// Take two logged from inside the stalled frame. Each host: log line costs
// ~2.3 ms, five lines pushed the frame past the 22 ms trigger, which fired
// another dump on the NEXT frame, and so on - ten "stalls" all reading a
// near-identical ~49.95 ms that was mostly the logging itself. Only the first
// sample was real.
//
// So: never log from the stalled frame. Remember the worst frame of each
// one-second window in memory, and emit ONE compact line afterwards.
namespace
{
    struct StallSample
    {
        uint64_t mFrameUs = 0;
        char     mTop[3][32] = {};
        float    mTopMs[3] = {};
    };

    StallSample sWorst;
    uint64_t    sPrevFrameUs  = 0;
    uint64_t    sWindowStart  = 0;
    uint64_t    sArmedAtUs    = 0;
}

void OctPostUpdate()
{
    const uint64_t now = SYS_GetTimeMicroseconds();
    const uint64_t frameUs = (sPrevFrameUs != 0) ? (now - sPrevFrameUs) : 0;
    sPrevFrameUs = now;

    if (sArmedAtUs == 0) { sArmedAtUs = now; sWindowStart = now; }
    if (now - sArmedAtUs < 15000000ull) return;      // skip loading

    // Keep the worst frame of this window, with its three costliest phases.
    if (frameUs > sWorst.mFrameUs)
    {
        sWorst.mFrameUs = frameUs;
        Profiler* prof = GetProfiler();
        if (prof != nullptr)
        {
            const std::vector<CpuStat>& stats = prof->GetCpuFrameStats();
            for (int slot = 0; slot < 3; ++slot)
            {
                const CpuStat* best = nullptr;
                for (size_t i = 0; i < stats.size(); ++i)
                {
                    // Skip ones already taken, and the all-encompassing wrappers.
                    bool taken = false;
                    for (int k = 0; k < slot; ++k)
                        if (strcmp(sWorst.mTop[k], stats[i].mName) == 0) taken = true;
                    if (taken) continue;
                    if (best == nullptr || stats[i].mTime > best->mTime) best = &stats[i];
                }
                if (best == nullptr) break;
                strncpy(sWorst.mTop[slot], best->mName, 31);
                sWorst.mTop[slot][31] = 0;
                sWorst.mTopMs[slot] = best->mTime;
            }
        }
    }

    // One line a second, emitted from a NORMAL frame, describing the worst.
    if (now - sWindowStart >= 1000000ull)
    {
        sWindowStart = now;
        if (sWorst.mFrameUs > 20000)
        {
            LogDebug("[PS2] worst frame %llu us | %s %.1f | %s %.1f | %s %.1f",
                     (unsigned long long)sWorst.mFrameUs,
                     sWorst.mTop[0], sWorst.mTopMs[0],
                     sWorst.mTop[1], sWorst.mTopMs[1],
                     sWorst.mTop[2], sWorst.mTopMs[2]);
        }
        sWorst = StallSample{};
    }
}

// Shutdown hooks - nothing PS2-specific to unwind here; GFX_Shutdown and
// AUD_Shutdown are driven by the engine.
void OctPreShutdown()  {}
void OctPostShutdown() {}

// Boot-device resolution lives in System_PS2.cpp; declared here rather than in
// a header because these are PS2-target internals, not engine API.
void        SYS_PS2_InitBootDevice(int argc, char** argv);
void        SYS_PS2_InitBootFilesystem();
bool        SYS_PS2_IsBootDeviceKnown();
const char* SYS_PS2_GetBootDevice();

int main(int argc, char** argv)
{
    // Work out which device we were launched from BEFORE anything opens a
    // file. Every subsequent asset/config path is prefixed with the result, so
    // getting this wrong means every open fails and the game looks empty.
    // Cheap and side-effect-free except for sceCdInit on a disc boot.
    SYS_PS2_InitBootDevice(argc, argv);

    // ---- Minimal SIF + IOP reset ------------------------------------------
    // SifInitRpc(0) wakes the EE↔IOP RPC bus. Required before any libloadfile
    // / fileXio / smap call. Cheap on PCSX2; ~1 ms on real hardware.
    SifInitRpc(0);

    // Reset and re-init the IOP — but ONLY on a host: boot.
    //
    // PCSX2 -elf and ps2link drop us in with the IOP in an unknown state, and
    // without a reset the next SifLoadFileInit() hangs. Booting from any real
    // device is the opposite case: whoever launched us has already set the IOP
    // up, and resetting it destroys their work.
    //
    // That is fatal under a loader such as OPL, which serves cdrom0: from its
    // OWN IOP modules — an SMB stack plus an emulated cdvdman. SifIopReset
    // reboots the IOP from the plain ROM image, so OPL's network and CD
    // emulation vanish and there is no longer any disc to read. The failure is
    // silent and total: init_scr() runs AFTER this point, so the screen stays
    // blank and not a single log line is ever printed.
    //
    // The same applies to mass: (USB/BDM), where the reset would unload the
    // block-device driver we were launched from.
    // Only reset when argv[0] EXPLICITLY said host:. "host:" is also the
    // fallback when argv carries no device at all, and a loader that launches
    // us without argv would otherwise be treated as a PCSX2 -elf boot and have
    // its IOP wiped — the exact failure this guard exists to prevent.
    const bool hostBoot = SYS_PS2_IsBootDeviceKnown() &&
                          (strncmp(SYS_PS2_GetBootDevice(), "host:", 5) == 0);

    // ...and even then, only if host: is not ALREADY serving.
    //
    // "host:" covers two very different launchers. Under PCSX2 -elf the device
    // is emulated by the emulator, so a reset costs nothing. Under ps2link it
    // is served by ps2link's OWN IOP modules over the network — and resetting
    // the IOP tears them down, which kills every asset load and the log file
    // while the game itself carries on running. Observed exactly that on
    // hardware: the VU1 test cubes drew fine and not one file could be opened.
    //
    // argv[0] cannot tell the two apart; both say "host:". So ask the device:
    // re-open the ELF we were launched from. If that works, host: is live and
    // must be left alone. If it does not, we are in the state the reset exists
    // to repair.
    bool hostAlive = false;
    if (hostBoot && argc > 0 && argv[0] != nullptr)
    {
        FILE* probe = fopen(argv[0], "rb");
        if (probe != nullptr) { fclose(probe); hostAlive = true; }
    }

    const bool doIopReset = hostBoot && !hostAlive;
    if (doIopReset)
    {
        while (!SifIopReset("", 0)) {}
        while (!SifIopSync())       {}
        SifInitRpc(0);
    }

    // sbv_patches lift the "module must be on protected media" restriction
    // so LoadModuleBuffer from EE RAM works (needed for shipping IRX in
    // Phase 3+: fileXio, audsrv, etc.). Safe to call always; PCSX2 and real
    // hardware both accept them.
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    // SifLoadFileInit opens the LoadFile RPC channel on the IOP — required
    // before any SifLoadModule / SifLoadModuleBuffer call. SifInitIopHeap
    // sets up the IOP-side allocator (audsrv allocates internal buffers
    // for its mixer on the IOP at init time). Order matters: LoadFileInit
    // first (RPC channel up), then IopHeap.
    SifLoadFileInit();
    SifInitIopHeap();

    // Boot-phase tty so we can see something before gsKit takes over.
    init_scr();

    Ps2_BootLog("[1] main() entered");
    // Print the boot device and whether the IOP was reset as early as the tty
    // allows — on a loader-launched disc boot these are the two facts that
    // decide whether anything else can possibly work.
    scr_printf("[1a] boot device: %s  hostAlive=%d  iopReset=%d\n",
               SYS_PS2_GetBootDevice(), hostAlive ? 1 : 0, doIopReset ? 1 : 0);
    Ps2_BootLog("[2] SIF + sbv patches applied");

    // ---- Load audio IRX stack --------------------------------------------
    // audsrv (the EE-side libaudsrv) RPCs into the audsrv IRX running on
    // the IOP, which drives the SPU2 via the LIBSD IRX. Load order is
    // mandatory: LIBSD first (from rom0:, present on every PS2 + PCSX2),
    // then audsrv from our embedded buffer, then audsrv_init(). Any
    // failure → audio offline but engine continues silently.
    {
        // Input prerequisites: SIO2MAN drives the controller/MC I/O ports,
        // PADMAN is the EE-side pad RPC client. Both ROM-resident on every
        // PS2 + PCSX2. Load BEFORE audsrv so libpad's padInit (called in
        // Input_PS2::INP_Initialize) finds PADMAN already running.
        const int sio2Ret = SifLoadModule("rom0:SIO2MAN", 0, nullptr);
        if (sio2Ret < 0) scr_printf("[2a] SIO2MAN load failed: %d\n", sio2Ret);
        const int padRet = SifLoadModule("rom0:PADMAN", 0, nullptr);
        if (padRet  < 0) scr_printf("[2a] PADMAN load failed: %d\n", padRet);

        const int libsdRet = SifLoadModule("rom0:LIBSD", 0, nullptr);
        if (libsdRet < 0)
        {
            scr_printf("[2a] LIBSD load failed: %d — audio offline\n", libsdRet);
        }
        else
        {
            // SifExecModuleBuffer takes EXPLICIT size + returns IOP init
            // result separately from the load result. This is what
            // ps2sdk-ports/SDL uses to embed audsrv via bin2c (the
            // canonical pattern — most reliable across PCSX2 versions
            // and real hardware).
            int audsrvInit = 0;
            const int audsrvRet = SifExecModuleBuffer(
                audsrv_irx,
                size_audsrv_irx,
                /*arg_len=*/0,
                /*args=*/nullptr,
                &audsrvInit);
            if (audsrvRet < 0)
            {
                scr_printf("[2a] audsrv exec failed: ret=%d init=%d size=%u — audio offline\n",
                           audsrvRet, audsrvInit, (unsigned)size_audsrv_irx);
            }
            else if (audsrvInit < 0)
            {
                scr_printf("[2a] audsrv module-init failed: %d (loaded ok, ret=%d) — audio offline\n",
                           audsrvInit, audsrvRet);
            }
            else
            {
                const int initRet = audsrv_init();
                if (initRet != 0)
                {
                    scr_printf("[2a] audsrv_init failed: %d — %s\n",
                               initRet, audsrv_get_error_string());
                }
                else
                {
                    scr_printf("[2a] audsrv up (libsd=%d audsrv=%d init=%d size=%u)\n",
                               libsdRet, audsrvRet, audsrvInit, (unsigned)size_audsrv_irx);
                }
            }
        }
    }

    // ---- Load memory-card IRX stack --------------------------------------
    // MCMAN drives the SIO2 transactions to the memory card hardware, MCSERV
    // is the EE-side RPC server. Both ROM-resident on every PS2 + PCSX2.
    // mcInit(MC_TYPE_XMC) brings the libmc EE side online and is required
    // before any mcOpen / mcRead / mcWrite call. MC_TYPE_XMC selects the
    // newer xmcman/xmcserv ABI surface — backwards-compatible with classic
    // mcman/mcserv so loading rom0:MCMAN works either way.
    {
        const int mcmanRet  = SifLoadModule("rom0:MCMAN",  0, nullptr);
        const int mcservRet = SifLoadModule("rom0:MCSERV", 0, nullptr);
        if (mcmanRet < 0 || mcservRet < 0)
        {
            scr_printf("[2b] MC IRX load failed (mcman=%d mcserv=%d) — saves offline\n",
                       mcmanRet, mcservRet);
        }
        else
        {
            const int mcInitRet = mcInit(MC_TYPE_XMC);
            if (mcInitRet < 0)
            {
                scr_printf("[2b] mcInit failed: %d — saves offline\n", mcInitRet);
            }
            else
            {
                scr_printf("[2b] libmc up (mcman=%d mcserv=%d mcInit=%d)\n",
                           mcmanRet, mcservRet, mcInitRet);
            }
        }
    }

    // ---- MMCE (SD2PSX / MemCard PRO): register mmce0: -----------------------
    // ORDER MATTERS. mmceman imports iomanX and registers its device through
    // it, not through plain ioman. Loading mmceman on its own fails with -200
    // and mmce0: never appears - confirmed on hardware, and confirmed by the
    // import table inside mmceman.irx (iomanx, sio2man, dmacman...).
    //
    // fileXio is the EE-side bridge to iomanX; without fileXioInit() the EE has
    // no way to reach devices that iomanX owns.
    //
    // All best-effort: a console with no MMCE device simply finds no hardware,
    // which must not stop boot.
    {
        int dummy = 0;
        const int iomanxRet  = SifExecModuleBuffer(
            iomanx_irx,  size_iomanx_irx,  0, nullptr, &dummy);
        const int filexioRet = SifExecModuleBuffer(
            filexio_irx, size_filexio_irx, 0, nullptr, &dummy);
        const int mmcemanRet = SifExecModuleBuffer(
            mmceman_irx, size_mmceman_irx, 0, nullptr, &dummy);
        const int mmcedrvRet = SifExecModuleBuffer(
            mmcedrv_irx, size_mmcedrv_irx, 0, nullptr, &dummy);

        const int fxInit = fileXioInit();

        scr_printf("[2c] MMCE: iomanX=%d fileXio=%d mmceman=%d mmcedrv=%d fxInit=%d\n",
                   iomanxRet, filexioRet, mmcemanRet, mmcedrvRet, fxInit);
    }

    // Spin up CDVD if we booted from a disc. Must be after the IOP reset and
    // module loading above — sceCdInit talks to cdvdman over the RPC bus, and
    // an earlier call would be undone by the reset.
    SYS_PS2_InitBootFilesystem();
    scr_printf("[2c] boot device: %s\n", SYS_PS2_GetBootDevice());

    Ps2_BootLog("[3] About to call GameMain()");
    GameMain(argc, argv);
    Ps2_BootLog("[4] GameMain() returned cleanly");

    return 0;
}

#endif // POLYPHASE_PLATFORM_ADDON
