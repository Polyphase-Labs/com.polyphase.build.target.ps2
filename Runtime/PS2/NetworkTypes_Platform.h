/**
 * @file NetworkTypes_Platform.h
 * @brief PS2 platform extension for the engine's `NetworkTypes.h` fork.
 *
 * Phase 0-2 stub. smap / sceNet stack is out of v1 scope. Map SocketHandle
 * to int32 (matches the other Unix-like platforms).
 */

#pragma once
#include <stdint.h>

typedef int32_t SocketHandle;
