/**
 * @file PS2GSTypes.h
 * @brief gsKit-compatible vertex / texture types for the PS2 graphics layer.
 *
 * Phase 0-2: placeholder. Phase 3+ fills in the real vertex layouts the
 * engine's static-mesh / skeletal-mesh / particle paths repack into for
 * gsKit submission.
 */

#pragma once

#include <stdint.h>

namespace ps2gs
{
    // Placeholder vertex used by the Phase 2 debug-triangle path. Real
    // engine repack types arrive in Phase 3.
    struct DebugVertex
    {
        float    x, y, z;
        uint32_t rgba;
    };
}
