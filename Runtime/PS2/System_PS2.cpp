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
#include <timer.h>
#include <debug.h>          // scr_printf
#include <delaythread.h>    // DelayThread
#include <unistd.h>         // rmdir, etc.
#include <libmc.h>          // mcOpen/mcRead/mcWrite/mcSync — memory card I/O
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

void SYS_Initialize()
{
    if (sInitialized) return;
    sInitialized = true;
    LogDebug("System_PS2: initialised (EE @ 294MHz, IOP @ 33MHz, GS w/ 4 MB VRAM)");
}

void SYS_Shutdown()
{
    sInitialized = false;
}

void SYS_Update()
{
    // PS2 has no per-frame callback drain analogue to PSP's sceKernelCheckCallback.
    // The EE kernel handles interrupts asynchronously; nothing to do here.
}

// =========================================================================
// Paths
// =========================================================================

std::string SYS_GetExecutablePath()
{
    // No introspection API for "where did this ELF come from". Return a
    // conventional path; engine consumers only need this for debug logs.
    return "host:polyphase.elf";
}

std::string SYS_GetPolyphasePath()
{
    return "host:";
}

std::string SYS_GetCurrentDirectoryPath()
{
    return "host:";
}

std::string SYS_GetAbsolutePath(const std::string& relativePath)
{
    if (relativePath.size() >= 5 && relativePath.compare(0, 5, "host:") == 0) return relativePath;
    if (relativePath.size() >= 7 && relativePath.compare(0, 7, "cdrom0:") == 0) return relativePath;
    if (relativePath.size() >= 4 && relativePath.compare(0, 4, "mc0:") == 0) return relativePath;
    if (relativePath.size() >= 4 && relativePath.compare(0, 4, "mc1:") == 0) return relativePath;
    return SYS_GetPolyphasePath() + relativePath;
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
        if (HasDevicePrefix(path)) return path;
        static char buf[512];
        snprintf(buf, sizeof(buf), "host:%s", path);
        return buf;
    }
}

bool SYS_DoesFileExist(const char* path, bool /*isAsset*/)
{
    if (path == nullptr) return false;
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

    const char* resolved = WithHostPrefix(path);
    FILE* f = fopen(resolved, "rb");
    if (f == nullptr)
    {
        LogWarning("SYS_AcquireFileData: fopen failed for '%s'", resolved);
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return; }

    uint32_t actual = (uint32_t)size;
    if (maxSize > 0 && actual > (uint32_t)maxSize) actual = (uint32_t)maxSize;

    outData = (char*)malloc(actual);
    if (outData == nullptr) { fclose(f); return; }

    const size_t read = fread(outData, 1, actual, f);
    fclose(f);
    outSize = (uint32_t)read;
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

void SYS_CopyFile(const char* sourcePath, const char* destPath)
{
    if (sourcePath == nullptr || destPath == nullptr) return;
    FILE* src = fopen(sourcePath, "rb");
    if (!src) return;
    FILE* dst = fopen(destPath, "wb");
    if (!dst) { fclose(src); return; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
    {
        if (fwrite(buf, 1, n, dst) != n) break;
    }
    fclose(src);
    fclose(dst);
}

void SYS_CopyDirectory(const char* /*sourceDir*/, const char* /*destDir*/) {}
bool SYS_CopyDirectoryRecursive(const std::string& /*sourceDir*/, const std::string& /*destDir*/) { return false; }
void SYS_MoveDirectory(const char* sourceDir, const char* destDir)
{
    if (sourceDir && destDir) rename(sourceDir, destDir);
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
    t.initial_priority = 32;     // mid-priority, same band as standard threads

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
    fclose(f);
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
static const char* sLogFilePath = "host:ps2-addon.log";
static FILE*       sLogFile     = nullptr;

void Ps2_AppendLogLineRaw(const char* line)
{
    if (line == nullptr) return;

    if (sLogFile == nullptr)
    {
        // Open with "w" on first use — truncate stale logs from prior runs
        // so a fresh launch doesn't append to a 100 MB file.
        sLogFile = fopen(sLogFilePath, "w");
        if (sLogFile != nullptr)
        {
            // Default newlib stdio uses full buffering; line buffer instead
            // so tailing the file while the game runs shows live progress.
            setvbuf(sLogFile, nullptr, _IOLBF, 0);
        }
    }

    if (sLogFile != nullptr)
    {
        fputs(line, sLogFile);
        fputc('\n', sLogFile);
        // Don't fflush on every line — _IOLBF already flushes on '\n'.
    }

    // Boot-tty mirror so very-early-boot crashes leave on-screen evidence.
    scr_printf("%s\n", line);
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
