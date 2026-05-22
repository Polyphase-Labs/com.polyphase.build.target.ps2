/**
 * @file Input_PS2.cpp
 * @brief PS2 input stubs (Phase 0-2). Full DualShock2 / libpad integration
 *        arrives in Phase 3 along with the IRX module load (padman.irx +
 *        sio2man.irx on the IOP side).
 *
 * Phase 0-2 still needs to satisfy the engine's INP_* symbol surface so the
 * link succeeds. We populate gamepad slot 0 as "connected, no buttons
 * pressed" each frame so engine code reading IsConnected/Just*Down patterns
 * behaves sanely without any actual input.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Input/Input.h"
#include "Input/InputUtils.h"
#include "Engine.h"
#include "Log.h"

void INP_Initialize()
{
    InputState& input = GetEngineState()->mInput;
    input.mGamepads[0].mType = GamepadType::Standard;
    input.mGamepads[0].mConnected = true;
    input.mNumControllers = 1;

    InputInit();
    LogDebug("Input_PS2: phase 0-2 stub (libpad bring-up deferred to Phase 3)");
}

void INP_Shutdown()
{
    InputShutdown();
}

void INP_Update()
{
    // Phase 3 will read libpad here. For now just advance the prev/current
    // frame state so the engine's transition detection doesn't spuriously fire.
    InputAdvanceFrame();
}

void INP_SetCursorPos(int32_t /*x*/, int32_t /*y*/) {}
void INP_ShowCursor(bool /*show*/) {}
void INP_LockCursor(bool /*lock*/) {}
void INP_TrapCursor(bool /*trap*/) {}
void INP_TrapCursorToRect(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/) {}

const char* INP_ShowSoftKeyboard(bool /*show*/) { return nullptr; }
bool INP_IsSoftKeyboardShown() { return false; }

#endif // POLYPHASE_PLATFORM_ADDON
