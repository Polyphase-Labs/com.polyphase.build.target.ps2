/**
 * @file PS2GSUtils.cpp
 * @brief gsKit helper bodies.
 *
 * Phase 0-2 placeholder. Phase 3+ implements the actual helpers; this file
 * exists so PS2GSUtils.h has a TU to satisfy any future external refs.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "PS2GSUtils.h"
#include "Engine.h"

#include <gsKit.h>

namespace ps2gs
{
    GSGLOBAL* GetGsGlobal()
    {
        // mGsGlobal is stored as void* in SystemState to avoid dragging
        // gsKit into the engine-facing fork header. Cast back here.
        return static_cast<GSGLOBAL*>(GetEngineState()->mSystem.mGsGlobal);
    }
}

#endif
