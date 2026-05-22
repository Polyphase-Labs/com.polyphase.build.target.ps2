/**
 * @file SystemTypes_Platform.h
 * @brief PS2 platform extension for the engine's `SystemTypes.h` fork.
 *
 * Picked up automatically when `POLYPHASE_PLATFORM_ADDON=1` is defined at
 * compile time. ActionManager generates a bridge header at
 * `<projectDir>/Generated/PolyphasePlatform_SystemTypes.h` that includes
 * this file via absolute path. Makefile_PS2 puts the Generated/ dir on the
 * include path.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

// PS2SDK threading. For filesystem we use POSIX <dirent.h> — modern PS2SDK's
// newlib port explicitly refuses direct fileXio includes ("Using fio/fileXio
// functions directly in the newlib port will lead to problems. Use posix
// function calls instead.") The newlib backend routes POSIX calls through
// fileXio under the hood when the IRX is loaded; Phase 0-2 stubs directory
// iteration anyway.
#include <kernel.h>
#include <dirent.h>

// ----- Threading typedefs --------------------------------------------------
// PS2SDK thread / sema IDs are plain ints returned from CreateThread /
// CreateSema. Thread entry functions return `int` (the standard MIPS ABI),
// so THREAD_RETURN resolves to `return 0;` — we do NOT set
// POLYPHASE_PLATFORM_ADDON_VOID_THREAD_RETURN.
typedef int  ThreadObject;
typedef int  MutexObject;
typedef int  ThreadFuncRet;

// ----- DirEntry injection --------------------------------------------------
// POSIX directory iteration: DIR* handle from opendir, struct dirent from
// readdir. Phase 0-2 stubs the actual iteration; this declaration just keeps
// DirEntry compiling.
#define POLYPHASE_PLATFORM_ADDON_DIRENTRY_MEMBERS \
    DIR*           mDirHandle = nullptr; \
    struct dirent  mLastDirent = {};

// ----- SystemState injection -----------------------------------------------
// PS2-specific window/display state. Most "windowing" concepts don't apply
// (PS2 has a fixed video output; resolution depends on region). We track the
// quit flag for the main loop, a background flag (placeholder; PS2 has no
// real background state), and a `void*` for the gsKit master context (cast
// to GSGLOBAL* inside Graphics_PS2GS.cpp where <gsKit.h> is included). Using
// `void*` instead of `struct GSGLOBAL;` because gsKit defines GSGLOBAL as a
// typedef-of-struct-gsGlobal — forward declaring it as a struct conflicts.
#define POLYPHASE_PLATFORM_ADDON_SYSTEMSTATE_MEMBERS \
    bool     mQuitRequested  = false; \
    bool     mInBackground   = false; \
    void*    mGsGlobal       = nullptr; \
    int32_t  mGsFrameCounter = 0;
