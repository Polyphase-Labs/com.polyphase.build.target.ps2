/**
 * @file System_PS2.cpp
 * @brief PS2-side implementation of the engine's SYS_* surface.
 *
 * Phase 0-2 baseline:
 *   - File I/O routed through newlib's stdio. PS2SDK's newlib backend
 *     transparently handles host:/, cdrom0:/, mc0:/ path prefixes (host: is
 *     active under PCSX2 -elf boot; cdrom0:/ for ISO boot). Directory
 *     iteration is stubbed (no entries) until Phase 3 wires fileXio properly.
 *   - Threading via PS2SDK kernel primitives: CreateThread + CreateSema (the
 *     binary-semaphore-as-mutex pattern, same as PSP).
 *   - Time via GetSystemTime — returns ticks since EE boot, convertible to
 *     microseconds via dividing by the kBUSCLK constant in psp-style code
 *     paths. The simpler kernel.h call is iGetSystemTime() which returns
 *     ticks; we scale to microseconds using 1 µs ≈ 147.456 ticks (EE bus
 *     clock 147.456 MHz).
 *   - No memory-card / save-data wired yet (defer to Phase 3 with libmc).
 *   - All window-state SYS_* is no-op (PS2 has a fixed video output).
 *
 * Built only when POLYPHASE_PLATFORM_ADDON is defined.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "System/System.h"
#include "Engine.h"
#include "Stream.h"
#include "Log.h"
#include "Utilities.h"

#include <kernel.h>
#include <loadfile.h>     // LoadExecPS2 - remote reload
#include <dirent.h>       // opendir/readdir - list a device when a path fails
#include <unistd.h>       // sbrk - heap high-water reporting
#include <timer.h>
#include <debug.h>          // scr_printf
#include <delaythread.h>    // DelayThread
#include <unistd.h>         // rmdir, etc.
#include <libmc.h>          // mcOpen/mcRead/mcWrite/mcSync — memory card I/O
#include <libcdvd.h>        // sceCdInit — required before any cdrom0: file open
#include <fcntl.h>          // O_RDONLY / O_WRONLY / O_CREAT / O_TRUNC for mcOpen modes

#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <sys/time.h>
#include <sys/stat.h>

static bool sInitialized = false;

// =========================================================================
// Lifecycle
// =========================================================================

// ---- EE-side SIF RPC serialisation ---------------------------------------
// The audio mixer runs on its OWN EE thread and polls audsrv_available() every
// 2 ms -- ~500 SIF RPCs a second, from boot, forever. The main thread loads
// every asset over host:, which is also SIF RPC (ps2link fileio). ps2sdk's RPC
// packet pool is shared between all EE callers and is NOT guarded, so two
// threads issuing RPCs concurrently can hand the same packet to
// rpc_packet_free twice.
//
// Observed: three intermittent load stalls at three different points (no error,
// no crash, just stops), and -- once logging added a third RPC into the file
// path -- a hard crash in _request_end (sifrpc.c:318) with BadVAddr 0x00000010,
// a null packet dereference.
//
// This serialises the two subsystems we control. Granularity is per stdio call,
// not per file, so a multi-megabyte asset read cannot starve the mixer.
static int sSifSema = -1;

void Ps2_SifLockInit()
{
    if (sSifSema >= 0) return;
    ee_sema_t s = {};
    s.init_count = 1;
    s.max_count  = 1;
    sSifSema = CreateSema(&s);
    if (sSifSema < 0) LogError("Ps2_SifLockInit: CreateSema failed (%d)", sSifSema);
}
void Ps2_SifLock()   { if (sSifSema >= 0) WaitSema(sSifSema); }
void Ps2_SifUnlock() { if (sSifSema >= 0) SignalSema(sSifSema); }

namespace
{
    // Scoped form for the file helpers below.
    struct SifGuard
    {
        SifGuard()  { Ps2_SifLock();   }
        ~SifGuard() { Ps2_SifUnlock(); }
    };
}

// ---- Heap accounting -------------------------------------------------------
// The EE has 32 MB and the loadable ELF is ~8.5 MB, so ~23 MB should be free.
// A 192 KB texture allocation failing means that is not what is happening, and
// until now the failure was invisible: SYS_AcquireFileData just returned a null
// buffer and the caller reported "Stream failed to read file".
uint32_t Ps2_HeapUsedBytes()
{
    // sbrk(0) is the current program break: everything below it is the ELF plus
    // whatever malloc has grown into. Cheap, and it needs no allocator hooks.
    const uint8_t* brk = (const uint8_t*)sbrk(0);
    return (uint32_t)(uintptr_t)brk;
}

// File I/O counters. Deliberately counters and NOT log lines: logging from
// inside this path issues a SIF RPC next to the one the read itself is making,
// which corrupted the shared RPC packet pool and crashed in _request_end.
// Incrementing an integer is safe; the totals are reported once a second from
// the render thread instead.
static uint32_t sIoFiles = 0;
static uint64_t sIoBytes = 0;
static uint64_t sIoUs    = 0;

void Ps2_GetIoStats(uint32_t* files, uint64_t* bytes, uint64_t* us)
{
    if (files != nullptr) *files = sIoFiles;
    if (bytes != nullptr) *bytes = sIoBytes;
    if (us    != nullptr) *us    = sIoUs;
    sIoFiles = 0; sIoBytes = 0; sIoUs = 0;
}

void Ps2_LogHeap(const char* tag)
{
    const uint32_t used = Ps2_HeapUsedBytes();
    const uint32_t total = 32u * 1024u * 1024u;
    LogDebug("[PS2] HEAP %s: break at %u KB of %u KB (%u KB left)",
             tag, used / 1024u, total / 1024u,
             (total > used) ? (total - used) / 1024u : 0u);
}

void SYS_Initialize()
{
    Ps2_SifLockInit();   // before ANY host: I/O or audio RPC
    if (sInitialized) return;
    sInitialized = true;
    LogDebug("System_PS2: initialised (EE @ 294MHz, IOP @ 33MHz, GS w/ 4 MB VRAM)");
}

void SYS_Shutdown()
{
    sInitialized = false;
}

#ifndef PS2_REMOTE_RELOAD
#define PS2_REMOTE_RELOAD 0
#endif

#if PS2_REMOTE_RELOAD
// ---- Remote reload -------------------------------------------------------
// Watch for a marker file over host: once a second. When it appears, hand the
// EE to whatever ELF path the file names -- so a new build can be launched from
// the PC without walking over to the console and hitting reset.
//
// reload.cmd is a TEXT file holding a path; it is the trigger, not the payload.
// The scripts keep it permanently present holding "none", and this poll disarms
// by writing "none" back rather than deleting it -- see the note below on what
// happens when a host: open finds nothing there.
//
// No purpose-built "reboot.elf" is needed. Useful targets:
//   host:BuildTarget-PS2.elf        relaunch the freshly built game (hot reload)
//   mc0:/ps2link_1.2/PS2LINK.ELF   back to ps2link, then execee as usual
//   mc0:/SYS_OSDMENU/osdmenu.elf    the FreeMcBoot OSD menu
//   rom0:OSDSYS                     bare BIOS browser; last resort, the one
//                                   target needing no filesystem at all
//
// This is NOT a ps2link server. The game has no TCP stack of its own
// (Network_PS2 is a stub, smap is never loaded), so it cannot accept an
// `execee` the way ps2link.elf does. What it does have is a working host:
// filesystem, courtesy of the ps2link IOP modules that stay resident while the
// game runs -- so a file IS the message.
//
// Costs one host: fopen per second (~1.7 ms on a ps2link rig) and compiles to
// nothing at all unless the 'Remote Reload' Target Option is checked.
void AUD_Shutdown();   // Audio_PS2.cpp (C++ linkage)

static void Ps2_PollRemoteReload()
{
    static const char* kMarker = "host:reload.cmd";

    static uint64_t sNextCheckUs = 0;
    const uint64_t now = SYS_GetTimeMicroseconds();
    if (now < sNextCheckUs) return;
    sNextCheckUs = now + 1000000ull;

    char path[256];
    path[0] = '\0';
    bool found = false;

    Ps2_SifLock();
    FILE* f = fopen(kMarker, "rb");
    if (f != nullptr)
    {
        found = true;
        const size_t n = fread(path, 1, sizeof(path) - 1, f);
        path[n] = '\0';
        fclose(f);
    }
    Ps2_SifUnlock();

    // One-shot diagnostics. Without these a failed open is indistinguishable
    // from "no reload requested", and the feature looks dead for either reason.
    static bool sReportedOk   = false;
    static bool sReportedFail = false;
    if (found && !sReportedOk)
    {
        sReportedOk = true;
        LogDebug("[PS2] remote reload: polling '%s' OK (%u bytes)",
                 kMarker, (unsigned)strlen(path));
    }
    else if (!found && !sReportedFail)
    {
        sReportedFail = true;
        LogWarning("[PS2] remote reload: cannot open '%s'. The marker must exist "
                   "and be a FILE in the directory ps2client was launched from "
                   "(run Tools/ps2run, which creates it).", kMarker);
    }

    if (!found) return;

    // Trim trailing whitespace a PC-side editor will have added.
    size_t len = strlen(path);
    while (len > 0 && (path[len - 1] == '\n' || path[len - 1] == '\r' ||
                       path[len - 1] == ' '  || path[len - 1] == '\t'))
    {
        path[--len] = '\0';
    }

    // "none" (what the scripts leave behind) and empty both mean "nothing to do".
    // Empty deliberately does NOT default to a reboot target any more: an empty
    // or unreadable marker is an ambiguous signal, and guessing wrong throws the
    // user out to the BIOS mid-session.
    if (len == 0 || strcmp(path, "none") == 0) return;

    const char* target = path;
    LogDebug("[PS2] remote reload requested -> '%s'", target);

    // Disarm by REWRITING the marker, never by deleting it. This poll must never
    // leave the file missing: opening a nonexistent path over ps2link's host:
    // creates a DIRECTORY of that name, which then blocks every future write
    // (the PC-side script gets "access denied") and blocks its own open, so the
    // feature silently disables itself. Observed exactly that.
    Ps2_SifLock();
    FILE* w = fopen(kMarker, "wb");
    if (w != nullptr)
    {
        fwrite("none", 1, 4, w);
        fclose(w);
    }
    Ps2_SifUnlock();
    if (w == nullptr)
    {
        LogWarning("[PS2] remote reload: could not disarm '%s' - the next run "
                   "will trigger again immediately", kMarker);
    }

    // Pre-flight the target. LoadExecPS2 is declared __attribute__((noreturn)),
    // so any error handling written AFTER it is dead code the compiler removes -
    // there is no way to report a bad path once the call is made. A typo would
    // simply take the console somewhere unrecoverable with nothing in the log.
    // So prove the file opens FIRST. rom0: is skipped: it is a BIOS device, not
    // a filesystem newlib can open, and it always exists.
    if (strncmp(target, "rom0:", 5) != 0)
    {
        Ps2_SifLock();
        FILE* probe = fopen(target, "rb");
        if (probe != nullptr) fclose(probe);
        Ps2_SifUnlock();
        if (probe == nullptr)
        {
            LogError("[PS2] remote reload: '%s' cannot be opened - NOT exec'ing it.", target);

            // Guessing at the right path has cost several round trips, so list
            // what is actually on the device instead. POSIX opendir, not fio* --
            // ps2sdk's newlib port #errors on direct fio/fileXio use.
            // Prefer listing the target's PARENT directory: that names the ELF
            // we actually wanted. Fall back to the device root when the parent
            // does not exist either.
            char dev[256];
            size_t n = strlen(target);
            while (n > 0 && target[n - 1] != '/') --n;   // strip the filename
            if (n > 0 && n < sizeof(dev))
            {
                memcpy(dev, target, n);
                dev[n] = '\0';
            }
            else
            {
                dev[0] = '\0';
            }

            Ps2_SifLock();
            DIR* probeDir = (dev[0] != '\0') ? opendir(dev) : nullptr;
            if (probeDir != nullptr) closedir(probeDir);
            Ps2_SifUnlock();

            if (probeDir == nullptr)
            {
                size_t d = 0;
                while (d < sizeof(dev) - 3 && target[d] != '\0' && target[d] != ':')
                {
                    dev[d] = target[d];
                    ++d;
                }
                dev[d++] = ':';
                dev[d++] = '/';
                dev[d]   = '\0';
            }

            Ps2_SifLock();
            DIR* dir = opendir(dev);
            Ps2_SifUnlock();
            if (dir == nullptr)
            {
                LogError("[PS2] remote reload: cannot list '%s' either - is that "
                         "device mounted? 'rom0:OSDSYS' always works.", dev);
            }
            else
            {
                LogDebug("[PS2] remote reload: contents of '%s':", dev);
                int shown = 0;
                for (;;)
                {
                    Ps2_SifLock();
                    struct dirent* ent = readdir(dir);
                    Ps2_SifUnlock();
                    if (ent == nullptr) break;
                    if (shown >= 200) { LogDebug("[PS2]     ... (truncated)"); break; }
                    if (ent->d_name[0] == '.') continue;   // . and ..
                    LogDebug("[PS2]     %s", ent->d_name);
                    ++shown;
                }
                Ps2_SifLock();
                closedir(dir);
                Ps2_SifUnlock();
            }
            return;
        }
    }

    // Stop the audio mixer before handing over. It is a separate EE thread
    // issuing SIF RPCs ~500x/s; leaving it running across LoadExecPS2 is asking
    // for a half-finished RPC to land in the next program's lap.
    AUD_Shutdown();
    // Nothing after this line runs: LoadExecPS2 is noreturn. Failure has to be
    // caught by the pre-flight open above, not reported here.
    LoadExecPS2(target, 0, nullptr);
}
#endif // PS2_REMOTE_RELOAD

void SYS_Update()
{
    // PS2 has no per-frame callback drain analogue to PSP's sceKernelCheckCallback.
    // The EE kernel handles interrupts asynchronously; nothing to do here.
#if PS2_REMOTE_RELOAD
    Ps2_PollRemoteReload();
#endif
}

// =========================================================================
// Paths
// =========================================================================

// =========================================================================
// Boot device resolution
//
// PS2SDK's newlib has NO default device: fopen("Config.ini") fails because
// nothing knows whether that means host:, cdrom0:, mass: or mc0:. Which one is
// correct depends entirely on how the ELF was launched, so it is discovered at
// startup from argv[0] — every PS2 loader passes the full boot path there:
//
//   PCSX2 -elf / ps2link : "host:...\BuildTarget-PS2.elf"  -> host:
//   Disc or ISO          : "cdrom0:\POLY0001.ELF;1"        -> cdrom0:
//   USB / SD via BDM     : "mass:/games/game.elf"          -> mass:
//
// Before this existed the prefix was hardcoded to "host:", which works only
// under PCSX2 and ps2link — so a burned disc or an SD-card boot would launch
// fine and then fail every single asset open, which looks exactly like an
// empty game.
// =========================================================================
namespace
{
    char sBootDevice[16] = "host:";   // includes the trailing ':'
    bool sBootIsCdrom    = false;
    // Does this device want a '/' between the prefix and the path?
    // host: does NOT (host:Config.ini). mass:, mc0: and mmce0: DO.
    // Getting this wrong makes every open fail with no error - decided by
    // probe at boot rather than assumed.
    bool sBootNeedsSlash = false;
    // Did argv[0] actually name a device, or is "host:" just the fallback?
    // The difference matters: resetting the IOP is only safe when we KNOW we
    // were launched by PCSX2 -elf / ps2link. Guessing wrong destroys a loader's
    // IOP modules and the console dies before it can print anything.
    bool sBootDeviceKnown = false;
}

void SYS_PS2_InitBootDevice(int argc, char** argv)
{
    if (argc > 0 && argv != nullptr && argv[0] != nullptr)
    {
        const char* a = argv[0];
        const char* colon = strchr(a, ':');
        // Guard the length so a path like "host:M:\..." takes the FIRST colon
        // (the device) and never the drive letter further along.
        if (colon != nullptr && (colon - a) > 0 && (colon - a) < (int)sizeof(sBootDevice) - 1)
        {
            const size_t n = (size_t)(colon - a) + 1;
            memcpy(sBootDevice, a, n);
            sBootDevice[n] = '\0';
            sBootDeviceKnown = true;
        }
    }

    sBootIsCdrom = (strncmp(sBootDevice, "cdrom", 5) == 0);
}

bool SYS_PS2_IsBootDeviceKnown() { return sBootDeviceKnown; }

// Second half of boot-device setup, split out because sceCdInit talks to
// cdvdman over the EE<->IOP RPC bus: it must run AFTER SifInitRpc and after the
// IOP reset (which would otherwise tear down the module we just initialised),
// whereas the argv[0] parse above must happen before anything opens a file.
void SYS_PS2_InitBootFilesystem()
{
    // Try one device in both separator forms. Returns true and records the form.
    auto tryDevice = [](const char* dev) -> bool
    {
        char path[96];
        for (int withSlash = 0; withSlash < 2; ++withSlash)
        {
            snprintf(path, sizeof(path), withSlash ? "%s/Config.ini" : "%sConfig.ini", dev);
            FILE* f = fopen(path, "rb");
            scr_printf("[1c]   try %-28s %s\n", path, (f != nullptr) ? "OK" : "-");
            if (f != nullptr)
            {
                fclose(f);
                // dev may BE sBootDevice when re-probing what argv gave us;
                // strncpy onto itself is undefined, so skip the copy then.
                if (dev != sBootDevice)
                {
                    strncpy(sBootDevice, dev, sizeof(sBootDevice) - 1);
                }
                sBootDevice[sizeof(sBootDevice) - 1] = '\0';
                sBootNeedsSlash  = (withSlash != 0);
                sBootIsCdrom     = (strncmp(dev, "cdrom", 5) == 0);
                sBootDeviceKnown = true;
                return true;
            }
        }
        return false;
    };

    // Config.ini sits at the root of every packaged build, so finding it proves
    // the device works AND that the content is actually there.
    //
    // argv[0] alone is not enough: it gave the right device here (mmce0:) while
    // the path SEPARATOR was wrong, and every open failed silently - a black
    // screen rendering an empty world. Hence probing both forms.
    if (sBootDeviceKnown && tryDevice(sBootDevice))
    {
        scr_printf("[1c] boot device %s confirmed (slash=%d)\n",
                   sBootDevice, sBootNeedsSlash ? 1 : 0);
        return;
    }

    // mc1: is deliberately absent: probing an EMPTY second card slot blocks for
    // seconds and looked like a hang on hardware (the trace stopped dead right
    // after mc0:). Nothing ships to slot 2, so the cost is not worth it.
    static const char* kCandidates[] = { "mmce0:", "mmce1:", "mass:", "mc0:", "host:" };
    for (size_t i = 0; i < sizeof(kCandidates) / sizeof(kCandidates[0]); ++i)
    {
        if (tryDevice(kCandidates[i]))
        {
            scr_printf("[1c] boot device resolved to %s (slash=%d)\n",
                       sBootDevice, sBootNeedsSlash ? 1 : 0);
            return;
        }
    }

    // cdrom0: LAST and only as a fallback: with no disc in the drive, spinning
    // up CDVD and opening a path blocks for a long time. That turned the probe
    // itself into an apparent hang.
    sceCdInit(CDVD_INIT_INIT);
    FILE* disc = fopen("cdrom0:\\SYSTEM.CNF;1", "rb");
    if (disc != nullptr)
    {
        fclose(disc);
        strcpy(sBootDevice, "cdrom0:");
        sBootIsCdrom     = true;
        sBootNeedsSlash  = false;   // the cdrom path is rebuilt separately
        sBootDeviceKnown = true;
        scr_printf("[1c] boot device resolved to cdrom0: (SYSTEM.CNF)\n");
        return;
    }

    scr_printf("[1c] NO BOOT DEVICE FOUND - Config.ini not on any device.\n");
    scr_printf("     Copy the CONTENTS of Build/PS2/Deploy to the device ROOT.\n");
}

const char* SYS_PS2_GetBootDevice() { return sBootDevice; }

std::string SYS_GetExecutablePath()
{
    // No introspection API for "where did this ELF come from" beyond argv[0],
    // which SYS_PS2_InitBootDevice already reduced to a device prefix. Engine
    // consumers only use this for debug logs.
    return std::string(sBootDevice) + "polyphase.elf";
}

std::string SYS_GetPolyphasePath()
{
    return sBootDevice;
}

std::string SYS_GetCurrentDirectoryPath()
{
    return sBootDevice;
}

// Defined further down, next to the rest of the path plumbing.
namespace { const char* WithHostPrefix(const char* path); }

std::string SYS_GetAbsolutePath(const std::string& relativePath)
{
    // Engine code calls this and then fopen()s the result DIRECTLY, without
    // going through SYS_DoesFileExist / SYS_ReadFile — so the normalisation has
    // to happen here too, not just in the fopen wrappers. Returning a bare
    // "cdrom0:" + "BuildTarget-PS2/Content.pak" produced a lowercase,
    // forward-slashed, un-versioned path that the CDVD driver cannot open.
    return WithHostPrefix(relativePath.c_str());
}

void SYS_ExplorerOpenDirectory(const std::string& /*dirPath*/) {}
void SYS_OpenFileWithDefaultApp(const std::string& /*filePath*/) {}
void SYS_SetWorkingDirectory(const std::string& /*dirPath*/) {}

// =========================================================================
// File I/O — newlib stdio routes through PS2SDK's host: backend on emulator
// and the disc / memory card on real hardware. fileXio's directory iteration
// is deferred to Phase 3.
//
// PS2SDK's newlib has NO default device. fopen("Config.ini") fails because
// "Config.ini" has no device prefix, and newlib doesn't know whether that
// means host:/cdrom0:/mc0: etc. Engine source passes paths like
// "Config.ini" or "BuildTarget-PS2/AssetRegistry.txt" without any prefix —
// so we prepend "host:" for any path that doesn't already have one.
//
// "host:" is the right default under PCSX2 -elf boot (PCSX2's host filesystem
// loader points at the directory containing the launched ELF). On real
// hardware booting from disc, "host:" wouldn't work and we'd need to swap
// the default to "cdrom0:" — that's Phase 3+ when we wire cdvd init.
// =========================================================================

namespace
{
    bool HasDevicePrefix(const char* path)
    {
        if (path == nullptr) return false;
        for (const char* p = path; *p && p - path < 16; ++p)
        {
            if (*p == ':') return true;
            if (*p == '/' || *p == '\\') return false;
        }
        return false;
    }

    // Returns either the original path (if it already has device:) or
    // "host:" + path. Uses a static buffer — PS2 in Phase 0-2 does file I/O
    // from one thread (main), so reentrancy isn't a concern. (Avoiding
    // thread_local because PS2SDK's TLS support for the EE is patchy.)
    //
    // Save-data routing: paths starting with "save/" get a "host:save/"
    // prefix in dev mode (writes land alongside the ELF, persist across
    // PCSX2 runs via host filesystem). On real hardware a future Phase 4
    // lift would route these to mc0:<discId>/ via libmc/mcserv.irx —
    // until then the host: prefix keeps Lua save/load Just Working under
    // PCSX2 -elf and -fastboot iso modes.
    const char* WithHostPrefix(const char* path)
    {
        if (path == nullptr) return nullptr;
        static char buf[512];

        const int  devLen = (int)strlen(sBootDevice);
        const bool hasOurDevice = (strncmp(path, sBootDevice, (size_t)devLen) == 0);

        // A path carrying some OTHER device (mc0:, rom0:, host: while booted
        // from disc) is deliberate — leave it exactly as the caller wrote it.
        if (!hasOurDevice && HasDevicePrefix(path)) return path;

        if (!sBootIsCdrom)
        {
            if (hasOurDevice) return path;
            snprintf(buf, sizeof(buf), sBootNeedsSlash ? "%s/%s" : "%s%s",
                     sBootDevice, path);
            return buf;
        }

        // IMPORTANT: paths that ALREADY carry our own device still have to be
        // normalised. The engine composes some of them itself as
        // SYS_GetPolyphasePath() + relative, producing e.g.
        //     "cdrom0:BuildTarget-PS2/Content.pak"
        // and cdvdman silently drops the forward slashes, so the open turns
        // into "BuildTarget-PS2Content.pak" and fails. Strip our prefix back
        // off and rebuild the path properly rather than passing it through.
        const char* rel = hasOurDevice ? path + devLen : path;
        while (*rel == '/' || *rel == '\\') ++rel;

        // ISO9660 needs three transformations the other devices don't:
        //   * backslash separators
        //   * UPPERCASE (mkisofs uppercases every name; a lowercase lookup
        //     simply will not match)
        //   * a ";1" version suffix on the file component
        // e.g. "Assets/Scenes/SC_Default.oct"
        //   -> "cdrom0:\ASSETS\SCENES\SC_DEFAULT.OCT;1"
        int n = snprintf(buf, sizeof(buf), "%s\\%s", sBootDevice, rel);
        if (n < 0) return path;
        if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;

        // Start past the device prefix: "cdrom0:" is lowercase by convention and
        // uppercasing it to "CDROM0:" would stop it resolving. Only the path
        // that follows gets normalised.
        int lastSep = devLen;                       // the '\' we just inserted
        for (int i = devLen; i < n; ++i)
        {
            if (buf[i] == '/') buf[i] = '\\';
            if (buf[i] == '\\') lastSep = i;
            else if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] = (char)(buf[i] - 'a' + 'A');
        }

        // Only the file component takes ";1" — directories must not have it.
        // Skip if the caller already supplied a version.
        if (memchr(buf + lastSep + 1, ';', (size_t)(n - lastSep - 1)) == nullptr &&
            n + 2 < (int)sizeof(buf))
        {
            buf[n++] = ';';
            buf[n++] = '1';
            buf[n]   = '\0';
        }
        return buf;
    }
}

bool SYS_DoesFileExist(const char* path, bool /*isAsset*/)
{
    if (path == nullptr) return false;
    SifGuard g;
    FILE* f = fopen(WithHostPrefix(path), "rb");
    if (f) { fclose(f); return true; }
    return false;
}

void SYS_AcquireFileData(const char* path, bool /*isAsset*/, int32_t maxSize,
                         char*& outData, uint32_t& outSize)
{
    outData = nullptr;
    outSize = 0;
    if (path == nullptr) return;

    // Heap watermark trace: log only when the break has moved by >=256 KB since
    // the last report, so a 200-asset load yields a readable handful of lines
    // rather than one per file.
    {
        static uint32_t sLastReport = 0;
        const uint32_t used = Ps2_HeapUsedBytes();
        if (sLastReport == 0 || (used > sLastReport && used - sLastReport >= 256u * 1024u))
        {
            sLastReport = used;
            Ps2_LogHeap("loading");
        }
    }

    const char* resolved = WithHostPrefix(path);
    // DO NOT log here. On a ps2link rig a log line is a host: write, i.e. a
    // SIF RPC -- issuing one immediately before this fopen's own RPC, while
    // the audio thread streams over RPC too, corrupts the shared RPC packet
    // pool. It crashed in _request_end (sifrpc.c:318) freeing a null packet,
    // reproducibly, on reaching the streaming audio asset.
    const uint64_t ioT0 = SYS_GetTimeMicroseconds();
    ++sIoFiles;
    Ps2_SifLock();
    FILE* f = fopen(resolved, "rb");
    Ps2_SifUnlock();
    if (f == nullptr)
    {
        LogWarning("SYS_AcquireFileData: fopen failed for '%s'", resolved);
        return;
    }

    // File size: fstat FIRST, fseek/ftell only as a fallback.
    //
    // fseek(SEEK_END)+ftell is NOT reliable on cdrom0:. Measured on a real ISO:
    // SM_Cylinder.oct is 2621 bytes on disc (verified byte-identical with
    // isoinfo) yet ftell reported 7023. Trusting that over-reads by ~4.4 KB,
    // pulling in whatever sectors happen to follow the file, and hands Stream a
    // buffer with a bogus length — which surfaced first as
    // "ASSERT: stringSize <= MAX_STRING_SIZE" and then as a silent parse
    // failure. ISO9660 stores the exact length in the directory record, so
    // fstat is authoritative where it is implemented.
    Ps2_SifLock();
    long size = -1;
    struct stat st;
    if (fstat(fileno(f), &st) == 0 && st.st_size > 0)
    {
        size = (long)st.st_size;
    }

    long tellSize = -1;
    fseek(f, 0, SEEK_END);
    tellSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    Ps2_SifUnlock();

    if (size < 0) size = tellSize;              // fstat unavailable on this device
    if (size < 0) { fclose(f); return; }

    if (tellSize != size)
    {
        LogWarning("[PS2] '%s': fstat says %ld bytes but ftell says %ld — trusting fstat",
                   resolved, size, tellSize);
    }

    uint32_t actual = (uint32_t)size;
    if (maxSize > 0 && actual > (uint32_t)maxSize) actual = (uint32_t)maxSize;

    // 64-byte aligned, sector-rounded allocation.
    //
    // cdvdman serves a read that fits inside one 2048-byte sector out of its own
    // cache with a memcpy, so alignment does not matter there — but a read that
    // SPANS sectors is DMA'd straight into this buffer, and the DMAC needs the
    // destination aligned. malloc gives no such guarantee. That is exactly the
    // observed failure: every asset under 2048 bytes loaded off the disc, and
    // the first one over it (SM_Cylinder, 7023 bytes) wedged the read.
    //
    // Rounding the size up as well keeps the DMA from writing a partial
    // trailing burst into memory it does not own. SYS_ReleaseFileData's free()
    // is correct for memalign under newlib.
    const uint32_t allocSize = (actual + 63u) & ~63u;
    outData = (char*)memalign(64, allocSize ? allocSize : 64);
    if (outData == nullptr)
    {
        // Was silent. An out-of-memory here surfaced two layers up as
        // "Stream failed to read file", which reads like an I/O fault and sent
        // this investigation down the wrong path.
        LogError("[PS2] OUT OF MEMORY: '%s' needs %ld bytes", resolved, size);
        Ps2_LogHeap("at failure");
        Ps2_SifLock(); fclose(f); Ps2_SifUnlock();
        return;
    }

    // Read in a LOOP. A single fread is not sufficient on cdrom0:: the CDVD
    // driver serves 2048-byte sectors and newlib's glue over cdvdman can return
    // a short count when a request spans sector boundaries — which is why a
    // one-sector asset loaded fine off the disc while a four-sector one did not.
    //
    // A truncated buffer is worse than a failed read: Stream goes on to parse
    // whatever happens to follow as asset data, and the damage surfaces a long
    // way from here as "ASSERT: stringSize <= MAX_STRING_SIZE" in ReadString.
    // Read in bounded chunks, looping until satisfied or EOF.
    //
    // The loop itself is the important part: a single fread can return short and
    // silently truncate, which Stream then parses as asset data.
    //
    // On the chunk size: an earlier revision capped this at one 2048-byte sector
    // because multi-sector reads appeared to hang. That diagnosis was wrong —
    // the real fault was the ISO9660 12-character name collision, which had
    // SM_Cylinder.oct loading SM_Cylinder.DAE, so the *parse* died rather than
    // the read. ContentPak issues single freads of up to a megabyte through the
    // same device and they complete fine. A 64 KB chunk keeps the short-read
    // safety without paying per-call latency on every sector.
    const uint32_t kSector = 64 * 1024;
    uint32_t total = 0;
    int guard = 0;
    while (total < actual)
    {
        uint32_t want = actual - total;
        if (want > kSector) want = kSector;
        Ps2_SifLock();
        const size_t got = fread(outData + total, 1, (size_t)want, f);
        Ps2_SifUnlock();
        // Bring-up probe: names the exact iteration if a multi-sector read
        // stalls. Only fires for files that need more than one pass.
        if (got == 0) break;                    // EOF or hard error
        total += (uint32_t)got;
        if (++guard > 8192) { LogError("[PS2] read loop stuck on '%s'", resolved); break; }
    }
    Ps2_SifLock(); fclose(f); Ps2_SifUnlock();
    sIoBytes += total;
    sIoUs    += SYS_GetTimeMicroseconds() - ioT0;

    if (total != actual)
    {
        LogWarning("SYS_AcquireFileData: short read on '%s' — got %u of %u bytes",
                   resolved, (unsigned)total, (unsigned)actual);
    }
    outSize = total;
}

void SYS_ReleaseFileData(char* data)
{
    free(data);
}

bool SYS_CreateDirectory(const char* dirPath)
{
    if (dirPath == nullptr) return false;
    // newlib mkdir on PS2 routes to fileXio when available; for Phase 0-2
    // (no fileXio init) it'll fail silently. Save data lands in Phase 3.
    return mkdir(dirPath, 0777) == 0;
}

void SYS_RemoveDirectory(const char* dirPath)
{
    if (dirPath == nullptr) return;
    rmdir(dirPath);
}

// Phase 0-2 stub for directory iteration. AssetManager::Discover uses this
// to walk the cooked-assets tree on disk. On PCSX2 -elf boot the assets are
// in the host: working dir; AssetManager will fail to find them via this
// stub, which is fine for proving boot+graphics works. Phase 3 wires fileXio.
void SYS_OpenDirectory(const std::string& dirPath, DirEntry& outDirEntry)
{
    outDirEntry.mValid = false;
    outDirEntry.mDirHandle = nullptr;
    strncpy(outDirEntry.mDirectoryPath, dirPath.c_str(), MAX_PATH_SIZE);
    outDirEntry.mDirectoryPath[MAX_PATH_SIZE] = '\0';
}

void SYS_IterateDirectory(DirEntry& dirEntry)
{
    dirEntry.mValid = false;
}

void SYS_CloseDirectory(DirEntry& dirEntry)
{
    dirEntry.mDirHandle = nullptr;
    dirEntry.mValid = false;
}

// Returns true if the file was fully copied. Matches the engine's SYS_CopyFile
// contract (System.h) so packaging can detect a failed copy instead of silently
// shipping a broken build.
bool SYS_CopyFile(const char* sourcePath, const char* destPath)
{
    if (sourcePath == nullptr || destPath == nullptr) return false;
    FILE* src = fopen(sourcePath, "rb");
    if (!src) return false;
    FILE* dst = fopen(destPath, "wb");
    if (!dst) { fclose(src); return false; }

    bool copyOk = true;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
    {
        if (fwrite(buf, 1, n, dst) != n) { copyOk = false; break; }
    }
    if (ferror(src)) copyOk = false;

    fclose(src);
    fclose(dst);
    return copyOk;
}

void SYS_CopyDirectory(const char* /*sourceDir*/, const char* /*destDir*/) {}
bool SYS_CopyDirectoryRecursive(const std::string& /*sourceDir*/, const std::string& /*destDir*/) { return false; }
bool SYS_MoveDirectory(const char* sourceDir, const char* destDir)
{
    if (sourceDir && destDir) return rename(sourceDir, destDir) == 0;
    return false;
}
void SYS_MoveFile(const char* sourcePath, const char* destPath)
{
    if (sourcePath && destPath) rename(sourcePath, destPath);
}
void SYS_RemoveFile(const char* path)
{
    if (path) remove(path);
}
bool SYS_Rename(const char* oldPath, const char* newPath)
{
    if (oldPath == nullptr || newPath == nullptr) return false;
    return rename(oldPath, newPath) == 0;
}

std::vector<std::string> SYS_OpenFileDialog() { return {}; }
std::string SYS_SaveFileDialog() { return ""; }
std::string SYS_SelectFolderDialog() { return ""; }

std::string SYS_GetFileName(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

// =========================================================================
// Threading
// =========================================================================

namespace
{
    // PS2SDK's CreateThread takes a function pointer of type `void(*)(void*)`
    // (no return) and a void* arg directly. Engine's ThreadFuncFP returns
    // `ThreadFuncRet` (int on PS2). We trampoline through this entry that
    // discards the return value.
    struct Ps2ThreadShim
    {
        ThreadFuncFP mFunc;
        void*        mArg;
        int          mStackPad[1];  // reserved for future stack-buffer alloc
    };

    void Ps2ThreadEntry(void* arg)
    {
        Ps2ThreadShim* shim = static_cast<Ps2ThreadShim*>(arg);
        if (shim == nullptr || shim->mFunc == nullptr) return;
        shim->mFunc(shim->mArg);
        delete shim;
        ExitDeleteThread();
    }
}

ThreadObject* SYS_CreateThread(ThreadFuncFP func, void* arg)
{
    if (func == nullptr) return nullptr;

    Ps2ThreadShim* shim = new Ps2ThreadShim{ func, arg, {0} };

    // Allocate stack ourselves (16 KB, 16-byte aligned). Standard EE
    // homebrew thread stack size; the engine's worker threads (asset loader)
    // don't need anything larger.
    constexpr int kStackSize = 0x4000;
    void* stack = memalign(16, kStackSize);
    if (stack == nullptr) { delete shim; return nullptr; }

    ee_thread_t t;
    memset(&t, 0, sizeof(t));
    // ee_thread_t::func is typed `void*` in PS2SDK (lifted from a Sony struct
    // where the field is just a raw pointer slot). Cast through reinterpret to
    // satisfy GCC's -fpermissive-default behaviour.
    t.func           = reinterpret_cast<void*>(&Ps2ThreadEntry);
    t.stack          = stack;
    t.stack_size     = kStackSize;
    t.gp_reg         = &_gp;
    // EE thread priority: LOWER number = HIGHER priority.
    //   0x30 (48)  audio mixer  - must never be starved, underruns are audible
    //   0x38 (56)  this band    - background workers (asset loader, audio
    //                             streaming I/O). Above the game so a blocked
    //                             worker wakes promptly, below the mixer so it
    //                             can never preempt audio.
    //   0x40 (64)  main thread  - the game/render loop
    //
    // This was 32, which put every worker ABOVE the mixer. Enabling the audio
    // streaming thread then made it outrank the mixer it was feeding, and the
    // music glitched for the duration of each chunk read.
    t.initial_priority = 0x38;

    const int th = CreateThread(&t);
    if (th < 0)
    {
        free(stack);
        delete shim;
        LogError("SYS_CreateThread: CreateThread failed (rc=%d)", th);
        return nullptr;
    }

    if (StartThread(th, shim) < 0)
    {
        DeleteThread(th);
        free(stack);
        delete shim;
        return nullptr;
    }

    // Stack is owned by the kernel until DeleteThread is called; we'll free
    // it in SYS_DestroyThread.
    ThreadObject* out = new ThreadObject;
    *out = th;
    return out;
}

void SYS_JoinThread(ThreadObject* thread)
{
    if (thread == nullptr) return;
    // PS2SDK has no native "wait for thread end" — polling on thread state
    // is the standard idiom. Sleep briefly between polls to avoid burning EE.
    for (;;)
    {
        ee_thread_status_t status;
        if (ReferThreadStatus(*thread, &status) < 0) break;
        if (status.status == 0x10 /* THS_DORMANT */) break;
        // Yield ~1 ms.
        for (int i = 0; i < 10; ++i) RotateThreadReadyQueue(32);
    }
}

void SYS_DestroyThread(ThreadObject* thread)
{
    if (thread == nullptr) return;
    if (*thread >= 0) DeleteThread(*thread);
    delete thread;
}

MutexObject* SYS_CreateMutex()
{
    ee_sema_t s;
    memset(&s, 0, sizeof(s));
    s.init_count = 1;
    s.max_count  = 1;
    s.option     = 0;
    const int sema = CreateSema(&s);
    if (sema < 0)
    {
        LogError("SYS_CreateMutex: CreateSema failed (rc=%d)", sema);
        return nullptr;
    }
    MutexObject* out = new MutexObject;
    *out = sema;
    return out;
}

void SYS_LockMutex(MutexObject* mutex)
{
    if (mutex == nullptr || *mutex < 0) return;
    WaitSema(*mutex);
}

void SYS_UnlockMutex(MutexObject* mutex)
{
    if (mutex == nullptr || *mutex < 0) return;
    SignalSema(*mutex);
}

void SYS_DestroyMutex(MutexObject* mutex)
{
    if (mutex == nullptr) return;
    if (*mutex >= 0) DeleteSema(*mutex);
    delete mutex;
}

void SYS_Sleep(uint32_t milliseconds)
{
    // PS2SDK has no millisecond sleep primitive. Use the timer: 1 ms ≈
    // 147456 EE bus cycles. iSleep usec API: DelayThread takes microseconds.
    DelayThread(milliseconds * 1000);
}

// =========================================================================
// Time
// =========================================================================

uint64_t SYS_GetTimeMicroseconds()
{
    // GetTimerSystemTime returns ticks on the EE bus clock (147.456 MHz).
    // Microseconds = ticks * 1e6 / 147,456,000 = ticks * 1000 / 147456.
    // Use 64-bit math to avoid intermediate overflow at long uptimes.
    const uint64_t ticks = (uint64_t)GetTimerSystemTime();
    return (ticks * 1000ULL) / 147456ULL;
}

// =========================================================================
// Process exec — N/A on PS2.
// =========================================================================

void SYS_Exec(const char* /*cmd*/, std::string* output)
{
    if (output) output->clear();
}

// =========================================================================
// Memory
// =========================================================================

void* SYS_AlignedMalloc(uint32_t size, uint32_t alignment)
{
    return memalign(alignment, size);
}

void SYS_AlignedFree(void* pointer)
{
    free(pointer);
}

std::vector<MemoryStat> SYS_GetMemoryStats()
{
    std::vector<MemoryStat> stats;
    MemoryStat mainRam;
    mainRam.mName = "MainRAM";
    // PS2SDK has no introspection equivalent to PSP's sceKernelTotalFreeMemSize.
    // Report zero for now; Phase 3+ can sample via the heap walker if needed.
    mainRam.mBytesFree = 0;
    mainRam.mBytesAllocated = 0;
    stats.push_back(mainRam);

    MemoryStat vram;
    vram.mName = "VRAM";
    vram.mBytesAllocated = 0;
    vram.mBytesFree = 4u * 1024u * 1024u;   // GS has 4 MB total VRAM
    stats.push_back(vram);

    return stats;
}

float SYS_GetRAMUsage()    { return 0.0f; }
float SYS_GetVRAMUsage()   { return 0.0f; }
float SYS_GetRAM1Usage()   { return 0.0f; }
float SYS_GetRAM2Usage()   { return 0.0f; }
float SYS_GetCPUUsage()    { return 0.0f; }
float SYS_GetTotalRAM()    { return (float)(32u * 1024u * 1024u); }   // 32 MB main
float SYS_GetTotalVRAM()   { return (float)(4u * 1024u * 1024u); }    // 4 MB GS VRAM
float SYS_GetTotalRAM1()   { return SYS_GetTotalRAM(); }
float SYS_GetTotalRAM2()   { return 0.0f; }

// =========================================================================
// Save data — PS2 memory card via libmc (mcman + mcserv IRX loaded in
// Main_PS2.cpp). Saves live in mc0:/POLYPHASE/<saveName> on the card in
// slot 0; falls back to host:save/<saveName> when no card is present
// (PCSX2 dev workflow without a memory card configured).
//
// libmc API discipline:
//   - mcInit() was called once in Main_PS2 — required before any libmc call.
//   - Every mc* function except mcInit() is ASYNC. After calling one, you
//     MUST call mcSync(0, &cmd, &result) to block until completion. `result`
//     carries the call's return value (fd for mcOpen, byte count for
//     mcRead/Write, status code for mcMkDir, etc.).
//   - mcOpen accepts POSIX-style flags from <fcntl.h>: O_RDONLY, O_WRONLY,
//     O_RDWR, O_CREAT, O_TRUNC, O_APPEND.
//   - Paths are RELATIVE to the card root (no "mc0:" prefix) and start with
//     a forward slash, e.g. "/POLYPHASE/savefile.dat".
//   - mcGetInfo probes card status — `type==MC_TYPE_PS2` + `formatted==1`
//     means a usable PS2 memory card is present in the queried slot.
// =========================================================================

namespace
{
    // Folder name on the card. Must follow the PS2 Browser naming convention
    // to show up in the system browser / memory-card manager:
    //   - "B"            = save data folder
    //   - "XDATA-"       = homebrew/third-party data (vs. ASLUS/ESLES/ISLPS
    //                      for licensed-game saves with region prefix)
    //   - "POLY0001"     = 8-char product code (matches default ps2.discId
    //                      from the build profile). When users change discId
    //                      in their Polyphase build profile, this should
    //                      match — for v1 the engine doesn't plumb discId
    //                      to runtime, so it's hardcoded to the default. Edit
    //                      both places if you change the build-profile default.
    // Max 32 chars on PS2 (no enforcement here — keep "BXDATA-XXXXXXXX" form).
    constexpr const char* kMcSaveDir       = "/BXDATA-POLY0001";
    constexpr const char* kMcSaveDirName   = "BXDATA-POLY0001";   // no leading slash
    constexpr int         kMcPort          = 0;                    // slot 0 = primary memory card
    constexpr int         kMcSlot          = 0;
    constexpr const char* kIconSysFileName = "icon.sys";
    constexpr const char* kIconIcnFileName = "icon.icn";

    // Host fallback path — used when no memory card is present (PCSX2 -elf
    // boot without an MC configured, or any setup where mcInit silently
    // failed). Mirrors the original Phase-3 dev behaviour.
    inline std::string HostFallbackPath(const char* saveName)
    {
        std::string p = "save/";
        p += saveName;
        return p;
    }

    inline std::string McSavePath(const char* saveName)
    {
        std::string p = kMcSaveDir;
        p += "/";
        p += saveName;
        return p;
    }

    // Probe slot 0 once and cache the verdict. mcGetInfo is async and we
    // don't want to round-trip it on every Save/Read call (a Lua game can
    // spam SYS_DoesSaveExist 60 times per second polling for save data).
    // Refresh logic: mcSync's `result` returns 0 when the card hasn't been
    // swapped since the last call; non-zero means the card changed and the
    // verdict must be re-evaluated.
    enum class McProbeState : int { Untried = 0, Available = 1, Unavailable = 2 };
    McProbeState sMcState = McProbeState::Untried;

    bool McProbeCard()
    {
        // If we already determined Unavailable, don't waste time re-probing
        // every call. The user would need to insert a card AND we'd need to
        // see a swap event to know to re-probe — accept the rare case where
        // a card is hot-inserted at runtime by deferring that to a future
        // "force re-probe" hook. Reasonable for v1.
        if (sMcState == McProbeState::Available)   return true;
        if (sMcState == McProbeState::Unavailable) return false;

        int type = 0, freeKb = 0, formatted = 0;
        int cmd  = 0, result = 0;
        if (mcGetInfo(kMcPort, kMcSlot, &type, &freeKb, &formatted) < 0)
        {
            sMcState = McProbeState::Unavailable;
            return false;
        }
        mcSync(0, &cmd, &result);

        // mcSync `result` for mcGetInfo: 0 = same card as last call (rare on
        // a cold first call — usually returns a positive "card changed"
        // signal). Negative values are hard errors. type / formatted carry
        // the actual verdict.
        const bool ok = (type == MC_TYPE_PS2) && (formatted == 1);
        if (ok)
        {
            sMcState = McProbeState::Available;
            LogDebug("SaveData: memory card detected on port %d slot %d "
                     "(%d KB free, formatted=%d)",
                     kMcPort, kMcSlot, freeKb, formatted);
        }
        else
        {
            sMcState = McProbeState::Unavailable;
            LogWarning("SaveData: no usable memory card (type=%d formatted=%d) — "
                       "falling back to host:save/", type, formatted);
        }
        return ok;
    }

    // PS2 Browser metadata. Layout matches `mcIcon` in <libmc.h> (964 bytes).
    // Written verbatim into icon.sys. Format reverse-engineered from real
    // PS2 BIOS browsers + uLaunchELF; all field offsets are LE on PS2 EE.
    //
    // Title is "packed ASCII" — Shift-JIS single-byte range (0x20-0x7E)
    // matches ASCII, so writing plain ASCII bytes into the 68-byte buffer
    // displays correctly in the browser. Set nlOffset to the byte index
    // where line 2 of the title starts (or to the title length if no
    // line break is desired).
    //
    // Background color is per-corner RGBA, stored as 4×4 int32 in
    // bgCol[16] order: TL.r,TL.g,TL.b,TL.a, TR.r,TR.g,TR.b,TR.a,
    // BL.r,..., BR.r,.... Each component is 0..0x80 (NOT 0..0xFF) — the
    // browser treats values >0x80 as oversaturated/clipped.
    //
    // Lighting: 3 directional lights + ambient. Light vectors are XYZ
    // direction (W is ignored). Colors are RGB(intensity) 0.0..1.0.
    //
    // view/copy/del are filenames (relative to save dir) for the 3D icon
    // displayed in the browser list / during copy / during delete. All three
    // can point at the same file. We use "icon.icn" but DON'T ship one in
    // v1 — most browsers (PCSX2 MC editor, every real-hardware BIOS we've
    // tested) show the folder with a placeholder icon when the referenced
    // .icn doesn't exist. If a browser hides the folder, write a real .icn.
    struct McIconSys
    {
        uint8_t  head[4];           // "PS2D"
        uint16_t type;              // 0
        uint16_t nlOffset;          // byte offset where title line 2 begins
        uint32_t unknown2;
        uint32_t trans;             // background transparency 0..0x80
        int32_t  bgCol[16];         // 4 corners × RGBA components
        float    lightDir[12];      // 3 lights × XYZW
        float    lightCol[12];      // 3 lights × RGBA
        float    lightAmbient[4];   // ambient RGBA
        uint8_t  title[68];         // packed ASCII / Shift-JIS
        uint8_t  view[64];          // icon filename for list view
        uint8_t  copy[64];          // icon filename when copying
        uint8_t  del[64];           // icon filename when deleting
        uint8_t  unknown3[512];     // padding to 964 bytes
    };
    static_assert(sizeof(McIconSys) == 964, "icon.sys layout must be 964 bytes");

    void BuildIconSys(McIconSys& out)
    {
        memset(&out, 0, sizeof(out));
        out.head[0] = 'P'; out.head[1] = 'S'; out.head[2] = '2'; out.head[3] = 'D';
        out.type    = 0;
        out.trans   = 0x60;     // semi-opaque card background

        // Title: 2 lines. Line 1 = "Polyphase", line 2 = "Save Data".
        // The browser shows the title under the icon; nlOffset is the byte
        // offset within title[] where line 2 begins. We put line 1 in
        // bytes [0..16], then "Save Data" starting at byte 16.
        const char* line1 = "Polyphase";
        const char* line2 = "Save Data";
        size_t l1 = strlen(line1);
        size_t l2 = strlen(line2);
        memcpy(out.title, line1, l1);
        // Line break: PS2 browser renders nlOffset as where line 2 begins.
        // Pad up to a fixed offset of 16 so the alignment looks consistent.
        constexpr size_t kLine2Offset = 16;
        if (kLine2Offset + l2 <= sizeof(out.title))
        {
            memcpy(out.title + kLine2Offset, line2, l2);
            out.nlOffset = (uint16_t)kLine2Offset;
        }

        // Background gradient: dark blue (Polyphase brand-ish). Same color
        // all 4 corners → flat fill. Values 0..0x80 (0x80 = max).
        for (int c = 0; c < 4; ++c)
        {
            out.bgCol[c * 4 + 0] = 0x10;   // R
            out.bgCol[c * 4 + 1] = 0x20;   // G
            out.bgCol[c * 4 + 2] = 0x50;   // B
            out.bgCol[c * 4 + 3] = 0x80;   // A (opaque)
        }

        // Lights: one key light from upper-right, one fill from left, one
        // rim from behind. Standard 3-point setup. Ambient kept low so the
        // icon (when present) reads with depth.
        // Light 0 — key, white, from front-upper-right
        out.lightDir[0]  = 0.5f;  out.lightDir[1]  = 0.5f;  out.lightDir[2]  = 0.5f;  out.lightDir[3]  = 0.f;
        out.lightCol[0]  = 1.0f;  out.lightCol[1]  = 1.0f;  out.lightCol[2]  = 1.0f;  out.lightCol[3]  = 1.f;
        // Light 1 — fill, cool blue, from left
        out.lightDir[4]  = -0.5f; out.lightDir[5]  = 0.2f;  out.lightDir[6]  = 0.3f;  out.lightDir[7]  = 0.f;
        out.lightCol[4]  = 0.4f;  out.lightCol[5]  = 0.5f;  out.lightCol[6]  = 0.8f;  out.lightCol[7]  = 1.f;
        // Light 2 — rim, warm, from behind-right
        out.lightDir[8]  = 0.3f;  out.lightDir[9]  = -0.3f; out.lightDir[10] = -0.7f; out.lightDir[11] = 0.f;
        out.lightCol[8]  = 0.9f;  out.lightCol[9]  = 0.6f;  out.lightCol[10] = 0.3f;  out.lightCol[11] = 1.f;
        // Ambient
        out.lightAmbient[0] = 0.2f;
        out.lightAmbient[1] = 0.2f;
        out.lightAmbient[2] = 0.3f;
        out.lightAmbient[3] = 1.f;

        // Icon file references. All three states point at the same file —
        // simplifies asset shipping. The file may not exist; browsers
        // typically fall back to a placeholder icon in that case.
        strncpy((char*)out.view, kIconIcnFileName, sizeof(out.view) - 1);
        strncpy((char*)out.copy, kIconIcnFileName, sizeof(out.copy) - 1);
        strncpy((char*)out.del,  kIconIcnFileName, sizeof(out.del)  - 1);
    }

    // ---- libmc async-then-sync wrappers ------------------------------------
    // Every mc* function (except mcInit/mcGetInfo's blocking shape) is async;
    // pair each with mcSync(0, &cmd, &result) and surface `result` as the
    // call's logical return. Declared here (before the first user) so
    // McWriteIconSysIfMissing / McEnsureSaveDir below can call them.
    int McOpenSync(const std::string& path, int mode)
    {
        int cmd = 0, result = 0;
        if (mcOpen(kMcPort, kMcSlot, path.c_str(), mode) < 0) return -1;
        mcSync(0, &cmd, &result);
        return result;
    }

    int McReadSync(int fd, void* buf, int size)
    {
        int cmd = 0, result = 0;
        if (mcRead(fd, buf, size) < 0) return -1;
        mcSync(0, &cmd, &result);
        return result;
    }

    int McWriteSync(int fd, const void* buf, int size)
    {
        int cmd = 0, result = 0;
        if (mcWrite(fd, buf, size) < 0) return -1;
        mcSync(0, &cmd, &result);
        return result;
    }

    int McSeekSync(int fd, int offset, int whence)
    {
        int cmd = 0, result = 0;
        if (mcSeek(fd, offset, whence) < 0) return -1;
        mcSync(0, &cmd, &result);
        return result;
    }

    void McCloseSync(int fd)
    {
        int cmd = 0, result = 0;
        mcClose(fd);
        mcSync(0, &cmd, &result);
    }

    int McDeleteSync(const std::string& path)
    {
        int cmd = 0, result = 0;
        if (mcDelete(kMcPort, kMcSlot, path.c_str()) < 0) return -1;
        mcSync(0, &cmd, &result);
        return result;
    }

    bool McWriteIconSysIfMissing()
    {
        // Probe first — don't re-write icon.sys on every save (wastes EE↔IOP
        // RPC bandwidth and burns memory-card cycles, even though MC writes
        // are wear-leveled internally).
        std::string iconSysPath = kMcSaveDir;
        iconSysPath += "/";
        iconSysPath += kIconSysFileName;

        int existsFd = McOpenSync(iconSysPath, O_RDONLY);
        if (existsFd >= 0)
        {
            McCloseSync(existsFd);
            return true;        // already there, nothing to do
        }

        McIconSys icon;
        BuildIconSys(icon);

        const int fd = McOpenSync(iconSysPath, O_CREAT | O_TRUNC | O_WRONLY);
        if (fd < 0)
        {
            LogWarning("SaveData: failed to create %s (rc=%d) — browser visibility off",
                       iconSysPath.c_str(), fd);
            return false;
        }
        const int written = McWriteSync(fd, &icon, sizeof(icon));
        McCloseSync(fd);

        if (written != (int)sizeof(icon))
        {
            LogWarning("SaveData: short write of icon.sys (%d of %d bytes)",
                       written, (int)sizeof(icon));
            return false;
        }

        LogDebug("SaveData: wrote icon.sys (%d bytes) — folder %s is now browser-visible",
                 written, kMcSaveDirName);
        return true;
    }

    bool McEnsureSaveDir()
    {
        int cmd = 0, result = 0;
        if (mcMkDir(kMcPort, kMcSlot, kMcSaveDir) < 0) return false;
        mcSync(0, &cmd, &result);
        // result: 0 = created; negative = error (most commonly -4 "exists",
        // which is success for our purpose). Treat any non-fatal result as
        // OK and let the subsequent mcOpen surface the real failure.

        // Once the dir exists, lay down icon.sys so the PS2 system browser
        // (and PCSX2's Memory Card Editor) recognises the folder as a save
        // and displays it. Idempotent — only writes if icon.sys is missing.
        McWriteIconSysIfMissing();
        return true;
    }
}

bool SYS_ReadSave(const char* saveName, Stream& outStream)
{
    if (saveName == nullptr) return false;

    if (McProbeCard())
    {
        const std::string path = McSavePath(saveName);
        const int fd = McOpenSync(path, O_RDONLY);
        if (fd < 0)
        {
            LogWarning("SYS_ReadSave: mcOpen '%s' failed (rc=%d)", path.c_str(), fd);
            return false;
        }

        const int size = McSeekSync(fd, 0, SEEK_END);
        McSeekSync(fd, 0, SEEK_SET);
        if (size <= 0) { McCloseSync(fd); return false; }

        outStream.Resize((uint32_t)size);
        const int read = McReadSync(fd, outStream.GetData(), size);
        McCloseSync(fd);

        if (read < 0)
        {
            LogError("SYS_ReadSave: mcRead '%s' failed (rc=%d)", path.c_str(), read);
            return false;
        }

        LogDebug("Save read: %s (%d bytes) from mc0:%s", saveName, read, path.c_str());
        return read > 0;
    }

    // Host fallback (no memory card).
    if (!SYS_DoesSaveExist(saveName))
    {
        LogWarning("SYS_ReadSave: '%s' does not exist", saveName);
        return false;
    }
    const std::string path = HostFallbackPath(saveName);
    outStream.ReadFile(path.c_str(), /*isAsset=*/false);
    return outStream.GetSize() > 0;
}

bool SYS_WriteSave(const char* saveName, Stream& stream)
{
    if (saveName == nullptr) return false;

    if (McProbeCard())
    {
        McEnsureSaveDir();      // best-effort; mcMkDir failure cascades to mcOpen
        const std::string path = McSavePath(saveName);
        const int fd = McOpenSync(path, O_CREAT | O_TRUNC | O_WRONLY);
        if (fd < 0)
        {
            LogError("SYS_WriteSave: mcOpen '%s' failed (rc=%d)", path.c_str(), fd);
            return false;
        }

        const int size = (int)stream.GetSize();
        int written = 0;
        if (size > 0)
        {
            written = McWriteSync(fd, stream.GetData(), size);
        }
        McCloseSync(fd);

        if (written < 0 || (size > 0 && written != size))
        {
            LogError("SYS_WriteSave: mcWrite '%s' short (%d of %d bytes)",
                     path.c_str(), written, size);
            return false;
        }

        LogDebug("Save written: %s (%d bytes) to mc0:%s", saveName, size, path.c_str());
        return true;
    }

    // Host fallback (no memory card).
    const std::string path = HostFallbackPath(saveName);
    const bool ok = stream.WriteFile(path.c_str());
    if (ok)
    {
        LogDebug("Save written: %s (%u bytes) -> host:%s",
                 saveName, (unsigned)stream.GetSize(), path.c_str());
    }
    else
    {
        LogError("SYS_WriteSave: failed to write 'host:%s' "
                 "(does the 'save/' directory exist next to the ELF?)",
                 path.c_str());
    }
    return ok;
}

bool SYS_DoesSaveExist(const char* saveName)
{
    if (saveName == nullptr) return false;

    if (McProbeCard())
    {
        const std::string path = McSavePath(saveName);
        const int fd = McOpenSync(path, O_RDONLY);
        if (fd < 0) return false;
        McCloseSync(fd);
        return true;
    }

    // Host fallback.
    const std::string path = HostFallbackPath(saveName);
    FILE* f = fopen(WithHostPrefix(path.c_str()), "rb");
    if (f == nullptr) return false;
    Ps2_SifLock(); fclose(f); Ps2_SifUnlock();
    return true;
}

bool SYS_DeleteSave(const char* saveName)
{
    if (saveName == nullptr) return false;

    if (McProbeCard())
    {
        const std::string path = McSavePath(saveName);
        const int rc = McDeleteSync(path);
        if (rc < 0)
        {
            LogWarning("SYS_DeleteSave: mcDelete '%s' failed (rc=%d)", path.c_str(), rc);
            return false;
        }
        return true;
    }

    // Host fallback.
    const std::string path = HostFallbackPath(saveName);
    return remove(WithHostPrefix(path.c_str())) == 0;
}

void SYS_UnmountMemoryCard()
{
    // Force a fresh probe on next save op — handles the case where a user
    // swaps cards between save calls (rare but valid). PCSX2's MC config
    // doesn't hot-swap mid-session, so this primarily helps real hardware.
    sMcState = McProbeState::Untried;
}

// =========================================================================
// Clipboard — N/A
// =========================================================================

void SYS_SetClipboardText(const std::string& /*str*/) {}
std::string SYS_GetClipboardText() { return ""; }

// =========================================================================
// Logging / assertions / dialogs
// =========================================================================

// File-backed log. Lands at host:polyphase.log under PCSX2 -elf boot
// (PCSX2's working dir). On real hardware it would route through fileXio
// to mc0:/POLYPHASE/polyphase.log — that integration is Phase 3.
//
// PCSX2's host: fopen is slow (~milliseconds per call — EE↔host RPC
// roundtrip). Keep the file open once, flush after each write, instead of
// opening+closing per LogDebug. Without this, an engine that LogDebug's
// 50+ times per frame (renderer, lua, asset manager) grinds to <1 fps
// because each call burns 5-10ms on host:fopen.
// "ps2-addon.log" not "polyphase.log" — engine's own LogToFile opens
// fopen("<projectName>.log", "w") which falls back to "Polyphase.log" when
// projectName is empty at init time (before ReadEngineConfig runs). On
// case-insensitive Windows host: storage that collides with our log file,
// and two concurrent FILE* writes interleave/clobber each other (visible
// symptom: log lines appear duplicated with mismatched prefixes, then
// truncate mid-line, then go silent even though the engine keeps running).
// Log destination is chosen at first use from the BOOT DEVICE, not hardcoded.
// Under ps2link that is host:; booted from USB or the memory card it has to be
// that device or there is no log at all, and with the on-screen console off
// (see Main_PS2) a device run would otherwise be completely silent.
static char sLogFilePath[64] = "host:ps2-addon.log";
static FILE*       sLogFile     = nullptr;

// Log cost accounting. Every line is a synchronous host: write RPC (a network
// round trip under ps2link) plus a framebuffer text render, and BOTH happen
// outside the renderer's timing window -- so this cost is invisible in the
// frame profile even though it still lengthens the frame. File and screen are
// timed separately so a slow write can be told apart from a slow scr_printf.
static uint64_t sLogFileUs = 0;
static uint64_t sLogScrUs  = 0;
static uint32_t sLogLines  = 0;

// scr_printf renders every line into the framebuffer. That is worth ~0.6 ms
// per line and is only useful before the renderer owns the screen, so the
// graphics layer turns it off once it is up (Ps2_SetLogToScreen).
static bool     sLogToScreen = true;
// fopen on a failed device must be tried ONCE. Without this the sink retries
// the open on every single line -- harmless on host:, ruinous on cdrom0:,
// where a disc build has no host: file to open at all.
static bool     sLogOpenTried = false;

void Ps2_SetLogToScreen(bool enable)
{
    // Refuse to turn the screen mirror OFF while there is no log file to write
    // to. Otherwise a device boot where the log could not be opened (wrong boot
    // device, read-only media, driver not up) goes completely silent the moment
    // GFX_Initialize runs - no file, no screen, nothing to debug with.
    //
    // The file is opened lazily on the first log line, which happens before
    // GFX_Initialize, so by the time anything disables this we already know
    // whether it worked.
    if (!enable && sLogFile == nullptr) return;
    sLogToScreen = enable;
}

void Ps2_GetLogCost(uint64_t* fileUs, uint64_t* scrUs, uint32_t* lines)
{
    if (fileUs != nullptr) *fileUs = sLogFileUs;
    if (scrUs  != nullptr) *scrUs  = sLogScrUs;
    if (lines  != nullptr) *lines  = sLogLines;
    sLogFileUs = 0; sLogScrUs = 0; sLogLines = 0;
}

void Ps2_AppendLogLineRaw(const char* line)
{
    if (line == nullptr) return;

    if (sLogFile == nullptr && !sLogOpenTried)
    {
        sLogOpenTried = true;

        // Pick the destination now that the boot device is known.
        const char* dev = SYS_PS2_IsBootDeviceKnown() ? SYS_PS2_GetBootDevice() : "host:";
        if (strncmp(dev, "cdrom", 5) == 0)
        {
            // A disc is read-only. Try the memory card so a disc build still
            // leaves evidence behind.
            snprintf(sLogFilePath, sizeof(sLogFilePath), "mc0:/ps2-addon.log");
        }
        else if (strncmp(dev, "host:", 5) == 0)
        {
            snprintf(sLogFilePath, sizeof(sLogFilePath), "host:ps2-addon.log");
        }
        else
        {
            snprintf(sLogFilePath, sizeof(sLogFilePath), "%s/ps2-addon.log", dev);
        }
        // Open with "w" on first use — truncate stale logs from prior runs
        // so a fresh launch doesn't append to a 100 MB file.
        sLogFile = fopen(sLogFilePath, "w");
        scr_printf("[1b] log -> %s  (%s)\n", sLogFilePath,
                   (sLogFile != nullptr) ? "open" : "FAILED - screen only");
        if (sLogFile != nullptr)
        {
            // Default newlib stdio uses full buffering; line buffer instead
            // so tailing the file while the game runs shows live progress.
            setvbuf(sLogFile, nullptr, _IOLBF, 0);
        }
        else
        {
            // Nowhere to write. Fall back to the on-screen console so a device
            // run is not completely undiagnosable - it costs frame time, but a
            // silent failure costs more.
            Ps2_SetLogToScreen(true);
        }
    }

    if (sLogFile != nullptr)
    {
        const uint64_t fT0 = SYS_GetTimeMicroseconds();
        fputs(line, sLogFile);
        fputc('\n', sLogFile);
        // Don't fflush on every line — _IOLBF already flushes on '\n'.
        sLogFileUs += SYS_GetTimeMicroseconds() - fT0;
    }

    // Boot-tty mirror so very-early-boot crashes leave on-screen evidence.
    if (sLogToScreen)
    {
        const uint64_t sT0 = SYS_GetTimeMicroseconds();
        scr_printf("%s\n", line);
        sLogScrUs += SYS_GetTimeMicroseconds() - sT0;
    }
    ++sLogLines;
}

void SYS_Log(LogSeverity severity, const char* format, va_list arg)
{
    char buf[1024];
    vsnprintf(buf, sizeof(buf), format, arg);

    const char* sevTag = (severity == LogSeverity::Error)   ? "[E] "
                       : (severity == LogSeverity::Warning) ? "[W] "
                       :                                       "[D] ";

    char line[1100];
    snprintf(line, sizeof(line), "%s%s", sevTag, buf);
    Ps2_AppendLogLineRaw(line);
}

void SYS_Assert(const char* exprString, const char* fileString, uint32_t lineNumber)
{
    scr_printf("ASSERT: %s\n  %s:%u\n", exprString, fileString, (unsigned)lineNumber);
    printf("ASSERT: %s at %s:%u\n", exprString, fileString, (unsigned)lineNumber);
    fflush(stdout);
    // Fall through; PS2 falling out of main returns to BIOS (real HW) or
    // exits PCSX2 -batch.
    SleepThread();
}

void SYS_Alert(const char* message)
{
    scr_printf("ALERT: %s\n", message);
    printf("ALERT: %s\n", message);
    fflush(stdout);
}

void SYS_UpdateConsole() {}

int32_t SYS_GetPlatformTier()
{
    return 0;
}

// =========================================================================
// Window — all no-ops on PS2 (fixed video output; resolution is the GS mode).
// =========================================================================

void SYS_SetWindowTitle(const char* /*title*/) {}
void SYS_SetWindowIcon(const char* /*iconPath*/) {}
bool SYS_DoesWindowHaveFocus() { return true; }
void SYS_SetScreenOrientation(ScreenOrientation /*orientation*/) {}
ScreenOrientation SYS_GetScreenOrientation() { return ScreenOrientation::Landscape; }
void SYS_SetFullscreen(bool /*fullscreen*/) {}
bool SYS_IsFullscreen() { return true; }
void SYS_SetWindowRect(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/) {}
void SYS_GetWindowRect(int32_t& outX, int32_t& outY, int32_t& outWidth, int32_t& outHeight)
{
    outX = 0; outY = 0;
    outWidth  = 640;
    outHeight = 448;     // NTSC; PAL set in Phase 3
}
bool SYS_IsWindowMaximized() { return true; }
void SYS_MaximizeWindow() {}

#endif // POLYPHASE_PLATFORM_ADDON
