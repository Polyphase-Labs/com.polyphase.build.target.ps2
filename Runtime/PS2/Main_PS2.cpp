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

#include "Engine.h"
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
}

void OctPreUpdate()    {}
void OctPostUpdate()   {}
void OctPreShutdown()  {}
void OctPostShutdown() {}

int main(int argc, char** argv)
{
    // ---- Minimal SIF + IOP reset ------------------------------------------
    // SifInitRpc(0) wakes the EE↔IOP RPC bus. Required before any libloadfile
    // / fileXio / smap call. Cheap on PCSX2; ~1 ms on real hardware.
    SifInitRpc(0);

    // Reset and re-init the IOP. Required when running from a host: boot
    // (PCSX2 -elf) because the IOP is in an unknown state — without these
    // the next SifLoadFileInit() hangs. On real hardware booting from disc
    // the BIOS does this for us.
    while (!SifIopReset("", 0)) {}
    while (!SifIopSync())       {}
    SifInitRpc(0);

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
    Ps2_BootLog("[2] SIF + sbv patches applied");

    // ---- Load audio IRX stack --------------------------------------------
    // audsrv (the EE-side libaudsrv) RPCs into the audsrv IRX running on
    // the IOP, which drives the SPU2 via the LIBSD IRX. Load order is
    // mandatory: LIBSD first (from rom0:, present on every PS2 + PCSX2),
    // then audsrv from our embedded buffer, then audsrv_init(). Any
    // failure → audio offline but engine continues silently.
    {
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

    Ps2_BootLog("[3] About to call GameMain()");
    GameMain(argc, argv);
    Ps2_BootLog("[4] GameMain() returned cleanly");

    return 0;
}

#endif // POLYPHASE_PLATFORM_ADDON
