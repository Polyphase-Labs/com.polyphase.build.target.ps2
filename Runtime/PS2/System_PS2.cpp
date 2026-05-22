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
// Save data — Phase 3 will wire libmc against mc0:/POLYPHASE/. Phase 0-2
// returns failure so engine save calls don't crash.
// =========================================================================

bool SYS_ReadSave(const char* /*saveName*/, Stream& /*outStream*/) { return false; }
bool SYS_WriteSave(const char* /*saveName*/, Stream& /*stream*/)   { return false; }
bool SYS_DoesSaveExist(const char* /*saveName*/)                   { return false; }
bool SYS_DeleteSave(const char* /*saveName*/)                      { return false; }
void SYS_UnmountMemoryCard() {}

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
