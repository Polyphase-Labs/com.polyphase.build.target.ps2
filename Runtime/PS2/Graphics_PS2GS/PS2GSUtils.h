/**
 * @file PS2GSUtils.h
 * @brief gsKit helper declarations.
 *
 * Phase 0-2 placeholder. Phase 3+ adds VRAM allocator wrappers, texture
 * upload helpers (with bufWidth alignment), and matrix conversions between
 * glm and gsKit's expected formats.
 */

#pragma once

#include <stdint.h>

// gsKit defines GSGLOBAL as a typedef of struct gsGlobal — forward declaring
// it as `struct GSGLOBAL;` conflicts. Forward declare the underlying struct
// tag instead; the typedef is resolved when callers also include <gsKit.h>.
struct gsGlobal;
typedef struct gsGlobal GSGLOBAL;

namespace ps2gs
{
    // Returns the configured gsKit master context, or nullptr before
    // GFX_Initialize is called.
    GSGLOBAL* GetGsGlobal();
}
