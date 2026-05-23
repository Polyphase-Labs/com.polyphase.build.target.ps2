/**
 * @file Input_PS2.cpp
 * @brief PS2 DualShock 2 input via libpad.
 *
 * Architecture:
 *   - Main_PS2.cpp loads SIO2MAN.irx + PADMAN.irx from rom0: at boot.
 *   - INP_Initialize calls padInit + padPortOpen(0,0) and waits for the
 *     pad to reach PAD_STATE_STABLE.
 *   - INP_Update reads padRead each frame, converts the inverted-bitmask
 *     btns field to engine GamepadState, and reports L2/R2 as analog axes
 *     (DS2 pressure-sensitive) plus the two thumbsticks.
 *
 * Face-button mapping mirrors PSP — Xbox-position convention so the same
 * GAMEPAD_A/B/X/Y indices give "confirm/cancel/west/north" semantically
 * regardless of platform.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Input/Input.h"
#include "Input/InputUtils.h"
#include "Engine.h"
#include "Log.h"
#include "Maths.h"

#include <libpad.h>
#include <kernel.h>

namespace
{
    constexpr int kPadPort = 0;
    constexpr int kPadSlot = 0;
    // libpad needs a 256-byte aligned buffer for each port. 64-byte align
    // is the documented minimum but 256 is safer across PS2SDK revisions.
    static char    sPadBuf[256] __attribute__((aligned(64)));
    static bool    sPadOpened   = false;
    static bool    sPadStable   = false;
    // padSetActAlign wants `const char[6]`. The byte values map small-motor
    // / large-motor actuators to logical slots — 0xff = unused.
    static const char sPadActAlign[6] = { 0, 1, char(0xff), char(0xff), char(0xff), char(0xff) };

    // Block until padGetState reports PAD_STATE_STABLE or we've waited too
    // long. Some controllers (especially via PCSX2's emulated pad) report
    // PAD_STATE_FINDPAD / PAD_STATE_EXECCMD for ~10 frames before settling.
    void WaitPadStable()
    {
        for (int i = 0; i < 600; ++i)   // ~10 sec max
        {
            const int state = padGetState(kPadPort, kPadSlot);
            if (state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1)
            {
                sPadStable = true;
                return;
            }
            // Sleep ~16 ms (one frame). Don't bake in EE_TICK_FREQ;
            // rough sleeps are fine for boot-phase polling.
            for (volatile int j = 0; j < 5000000; ++j) {}
        }
        LogWarning("Input_PS2: padGetState never reached STABLE — input offline");
    }
}

void INP_Initialize()
{
    InputState& input = GetEngineState()->mInput;
    input.mGamepads[0].mType = GamepadType::Standard;
    input.mGamepads[0].mConnected = false;   // updated after padPortOpen succeeds
    input.mNumControllers = 1;

    InputInit();

    if (padInit(0) != 1)
    {
        LogWarning("Input_PS2: padInit failed — input offline");
        return;
    }

    if (padPortOpen(kPadPort, kPadSlot, sPadBuf) == 0)
    {
        LogWarning("Input_PS2: padPortOpen(%d,%d) failed — input offline",
                   kPadPort, kPadSlot);
        return;
    }
    sPadOpened = true;

    WaitPadStable();
    if (!sPadStable) return;

    // Enable analog mode (DualShock 2). Some 3rd-party pads only support
    // digital — if SetMainMode fails we still read digital input below
    // and the analog axes just stay at zero.
    if (padInfoMode(kPadPort, kPadSlot, PAD_MODECURID, 0) != 0)
    {
        padSetMainMode(kPadPort, kPadSlot, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
        // Wait for the mode change to take effect.
        for (int i = 0; i < 100; ++i)
        {
            if (padGetReqState(kPadPort, kPadSlot) == PAD_RSTAT_COMPLETE) break;
            for (volatile int j = 0; j < 1000000; ++j) {}
        }
        // Enable pressure-sensitive buttons (DS2 only — DS1 will fail
        // silently).
        padSetActAlign(kPadPort, kPadSlot, sPadActAlign);
        padEnterPressMode(kPadPort, kPadSlot);
    }

    LogDebug("Input_PS2: libpad ready (port=%d slot=%d)", kPadPort, kPadSlot);
}

void INP_Shutdown()
{
    if (sPadOpened)
    {
        padPortClose(kPadPort, kPadSlot);
        sPadOpened = false;
    }
    padEnd();
    InputShutdown();
}

void INP_Update()
{
    InputAdvanceFrame();

    if (!sPadOpened || !sPadStable)
    {
        // Pad never came up — leave gamepad disconnected so engine UI nav
        // doesn't fire spurious "just-down" transitions.
        return;
    }

    InputState& input = GetEngineState()->mInput;
    GamepadState& gp = input.mGamepads[0];

    if (padGetState(kPadPort, kPadSlot) != PAD_STATE_STABLE)
    {
        gp.mConnected = false;
        return;
    }
    gp.mConnected = true;

    padButtonStatus data;
    const int ret = padRead(kPadPort, kPadSlot, &data);
    if (ret == 0) return;

    // libpad returns `btns` as inverted bitmask (pressed = 0). Invert
    // so a set bit means "button held" — matches the rest of the engine.
    const uint16_t b = (uint16_t)(0xffff ^ data.btns);

    // Face buttons — Xbox-position mapping (A=bottom, B=right, X=left, Y=top).
    gp.mButtons[GAMEPAD_A] = (b & PAD_CROSS)    ? 1 : 0;
    gp.mButtons[GAMEPAD_B] = (b & PAD_CIRCLE)   ? 1 : 0;
    gp.mButtons[GAMEPAD_X] = (b & PAD_SQUARE)   ? 1 : 0;
    gp.mButtons[GAMEPAD_Y] = (b & PAD_TRIANGLE) ? 1 : 0;

    // Shoulders / triggers — DS2 has analog L2/R2, treat L1/R1 as digital.
    gp.mButtons[GAMEPAD_L1] = (b & PAD_L1) ? 1 : 0;
    gp.mButtons[GAMEPAD_R1] = (b & PAD_R1) ? 1 : 0;
    gp.mButtons[GAMEPAD_L2] = (b & PAD_L2) ? 1 : 0;
    gp.mButtons[GAMEPAD_R2] = (b & PAD_R2) ? 1 : 0;

    // D-pad
    gp.mButtons[GAMEPAD_UP]    = (b & PAD_UP)    ? 1 : 0;
    gp.mButtons[GAMEPAD_DOWN]  = (b & PAD_DOWN)  ? 1 : 0;
    gp.mButtons[GAMEPAD_LEFT]  = (b & PAD_LEFT)  ? 1 : 0;
    gp.mButtons[GAMEPAD_RIGHT] = (b & PAD_RIGHT) ? 1 : 0;

    // System buttons + stick clicks
    gp.mButtons[GAMEPAD_START]  = (b & PAD_START)  ? 1 : 0;
    gp.mButtons[GAMEPAD_SELECT] = (b & PAD_SELECT) ? 1 : 0;
    gp.mButtons[GAMEPAD_HOME]   = 0;   // PS2 has no HOME button
    gp.mButtons[GAMEPAD_THUMBL] = (b & PAD_L3)     ? 1 : 0;
    gp.mButtons[GAMEPAD_THUMBR] = (b & PAD_R3)     ? 1 : 0;

    // Analog sticks — DS2 reports 0..255 with center ~128 (like PSP).
    // Engine convention: +Y = up, but libpad ljoy_v grows downward, so invert.
    constexpr float kAnalogDeadzone = 0.20f;
    auto applyDeadzone = [](float v) -> float {
        if (v >  kAnalogDeadzone) return (v - kAnalogDeadzone) / (1.0f - kAnalogDeadzone);
        if (v < -kAnalogDeadzone) return (v + kAnalogDeadzone) / (1.0f - kAnalogDeadzone);
        return 0.0f;
    };
    const float lx = (float(data.ljoy_h) - 128.0f) / 128.0f;
    const float ly = (float(data.ljoy_v) - 128.0f) / 128.0f;
    const float rx = (float(data.rjoy_h) - 128.0f) / 128.0f;
    const float ry = (float(data.rjoy_v) - 128.0f) / 128.0f;
    gp.mAxes[GAMEPAD_AXIS_LTHUMB_X] = glm::clamp(applyDeadzone( lx), -1.0f, 1.0f);
    gp.mAxes[GAMEPAD_AXIS_LTHUMB_Y] = glm::clamp(applyDeadzone(-ly), -1.0f, 1.0f);
    gp.mAxes[GAMEPAD_AXIS_RTHUMB_X] = glm::clamp(applyDeadzone( rx), -1.0f, 1.0f);
    gp.mAxes[GAMEPAD_AXIS_RTHUMB_Y] = glm::clamp(applyDeadzone(-ry), -1.0f, 1.0f);

    // DS2 pressure-sensitive analog triggers — l2_p / r2_p 0..255 each.
    gp.mAxes[GAMEPAD_AXIS_LTRIGGER] = float(data.l2_p) / 255.0f;
    gp.mAxes[GAMEPAD_AXIS_RTRIGGER] = float(data.r2_p) / 255.0f;

    // Stick-click virtual buttons stay at zero — the engine derives them
    // from the analog axes itself in InputPostUpdate.
    gp.mButtons[GAMEPAD_R_LEFT]  = 0;
    gp.mButtons[GAMEPAD_R_RIGHT] = 0;
    gp.mButtons[GAMEPAD_R_UP]    = 0;
    gp.mButtons[GAMEPAD_R_DOWN]  = 0;

    InputPostUpdate();
}

// PS2 doesn't surface cursor / mouse / soft-keyboard concepts the engine knows
// about. Symbols required for link.
void INP_SetCursorPos(int32_t /*x*/, int32_t /*y*/) {}
void INP_ShowCursor(bool /*show*/) {}
void INP_LockCursor(bool /*lock*/) {}
void INP_TrapCursor(bool /*trap*/) {}
void INP_TrapCursorToRect(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/) {}

const char* INP_ShowSoftKeyboard(bool /*show*/) { return nullptr; }
bool INP_IsSoftKeyboardShown() { return false; }

#endif // POLYPHASE_PLATFORM_ADDON
