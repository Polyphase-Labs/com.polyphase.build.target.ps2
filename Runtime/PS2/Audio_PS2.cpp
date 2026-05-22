/**
 * @file Audio_PS2.cpp
 * @brief PS2 audio stubs (Phase 0-2). SPU2 / libaudsrv arrives in Phase 3.
 *
 * audsrv runs as an IRX module on the IOP. The setup involves SifLoadModule
 * of audsrv.irx + audsrv_init, then an EE-side mixer thread that writes PCM
 * via audsrv_play_audio. Pattern is similar in shape to PSP's Audio_PSP.cpp
 * (mixer thread + voice table), but the actual API surface (audsrv_*) is
 * entirely different. Deferring it lets Phase 0-2 link without bringing in
 * the IRX dependency.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Audio/Audio.h"
#include "Audio/AudioConstants.h"
#include "Engine/Assets/SoundWave.h"
#include "Log.h"

#include <stdlib.h>
#include <string.h>

void AUD_Initialize()
{
    LogDebug("Audio_PS2: phase 0-2 stub (audsrv.irx + SPU2 bring-up deferred to Phase 3)");
}

void AUD_Shutdown() {}
void AUD_Update()   {}

void AUD_Play(uint32_t /*voiceIndex*/, SoundWave* /*soundWave*/, float /*volume*/,
              float /*pitch*/, bool /*loop*/, float /*startTime*/, bool /*spatial*/) {}
void AUD_Stop(uint32_t /*voiceIndex*/) {}
bool AUD_IsPlaying(uint32_t /*voiceIndex*/) { return false; }
void AUD_SetVolume(uint32_t /*voiceIndex*/, float /*leftVolume*/, float /*rightVolume*/) {}
void AUD_SetPitch(uint32_t /*voiceIndex*/, float /*pitch*/) {}

uint8_t* AUD_AllocWaveBuffer(uint32_t numBytes)
{
    // Phase 0-2: trivial malloc. Phase 3 will route through audsrv's
    // requirement for IOP-visible memory if the codec path needs it.
    return static_cast<uint8_t*>(malloc(numBytes));
}
void AUD_FreeWaveBuffer(void* buffer) { free(buffer); }
void AUD_ProcessWaveBuffer(SoundWave* /*soundWave*/) {}

// Streaming API — engine's video player and music streamers reach for it.
// Phase 0-2 makes them graceful no-ops so the engine doesn't crash; Phase 3
// wires streams through the audsrv mixer.
uint32_t AUD_OpenStream(uint32_t /*sampleRate*/, uint32_t /*numChannels*/, uint32_t /*bitsPerSample*/)
{
    return 0;
}
void     AUD_CloseStream(uint32_t /*streamId*/) {}
int32_t  AUD_SubmitStreamBuffer(uint32_t /*streamId*/, const uint8_t* /*data*/, uint32_t /*byteSize*/) { return 0; }
void     AUD_SetStreamVolume(uint32_t /*streamId*/, float /*volume*/) {}
void     AUD_SetStreamPaused(uint32_t /*streamId*/, bool /*paused*/) {}
void     AUD_FlushStream(uint32_t /*streamId*/) {}
uint64_t AUD_GetStreamPlayedSamples(uint32_t /*streamId*/) { return 0; }

#endif // POLYPHASE_PLATFORM_ADDON
