/**
 * @file Audio_PS2.cpp
 * @brief PS2 audio — software mixer feeding a single audsrv output channel.
 *
 * Architecture (mirrors Audio_PSP.cpp; memory: project_psp_audio_mixer):
 *   - Engine drives AUD_Play(voiceIndex, soundWave, ...) onto a voice table,
 *     parallel streams via AUD_OpenStream / AUD_SubmitStreamBuffer.
 *   - A mixer thread sleeps inside audsrv_play_audio. When the IOP-side
 *     audsrv consumes the buffer the call returns, the thread mixes voices +
 *     streams into a stereo S16LE block, and submits the next one.
 *   - Per-voice nearest-neighbour resampling via fractional cursor advanced
 *     by `rate = srcSampleRate * pitch / 44100` per output frame.
 *
 * audsrv background: audsrv is an IRX module that runs on the IOP and
 * exposes EE-side stubs (libaudsrv.a) for queueing PCM. Main_PS2.cpp loads
 * `audsrv.irx` via SifLoadModuleBuffer at boot — the IRX itself is embedded
 * into the ELF via bin2s in the Makefile. After audsrv_init() succeeds we
 * configure format with audsrv_set_format(44100, 16, 2) and start the mixer.
 *
 * Threading discipline:
 *   - sVoiceLock semaphore (initial=1, max=1) guards both sVoices and
 *     sStreams. Engine-side mutators take it briefly; mixer takes it for
 *     the duration of one buffer fill (~few hundred µs) and drops it before
 *     audsrv_play_audio (which blocks until the IOP has consumed a buffer).
 *   - SoundWave pointers in voices are weak refs — engine keeps the
 *     SoundWave alive via asset ref count.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Audio/Audio.h"
#include "Audio/AudioConstants.h"
#include "Engine/Assets/SoundWave.h"
#include "Log.h"

#include <kernel.h>
#include <audsrv.h>
#include <debug.h>       // scr_printf — IOP tty diagnostic, bypasses host: log buffering

#include <stdlib.h>
#include <string.h>
#include <malloc.h>

namespace
{
    constexpr int kOutputRate         = 44100;
    constexpr int kFramesPerBuffer    = 1024;          // ~23 ms @ 44.1 kHz
    constexpr int kSamplesPerBuffer   = kFramesPerBuffer * 2;  // stereo
    constexpr int kBytesPerBuffer     = kSamplesPerBuffer * 2; // S16
    // Master attenuation: was 0.5 (mirrors PSP) for clipping headroom
    // when N voices sum into one s32 lane. But the actual content we get
    // from the engine is already quiet — star_map_menu2 peaks at ~12% of
    // 8-bit full scale, then engine adds a 33% spatial multiplier, so a
    // 0.5 master kills another 2x → output ends up at ~2% s16 amplitude
    // which is inaudible through PCSX2's audio backend. Drop master to
    // 1.0 (no attenuation). The mixer still saturates per-sample so
    // multi-voice clipping is safe; we just don't pre-attenuate.
    constexpr float kMasterAtten      = 1.0f;

    struct Ps2Voice
    {
        // `volatile` on the lifecycle/position fields so the EE compiler
        // can't hoist reads out of the mixer's while loop or across the
        // WaitSema barrier. PS2's R5900 EE is single-core but with
        // aggressive register/load caching by gcc 15.x — without
        // volatile, gcc was hoisting `sVoices[0].active` to a register
        // at function entry and never reloading after the AUD_Play thread
        // updated memory.
        volatile bool   active        = false;
        const uint8_t*  pcmData       = nullptr;
        uint32_t        numFrames     = 0;
        uint32_t        sampleRate    = 0;
        uint8_t         numChannels   = 1;
        uint8_t         bitsPerSample = 16;
        bool            loop          = false;
        volatile double positionFrac  = 0.0;
        double          rate          = 1.0;
        int32_t         leftVolQ15    = 32768;
        int32_t         rightVolQ15   = 32768;
    };
    static Ps2Voice sVoices[AUDIO_MAX_VOICES];

    constexpr uint32_t kMaxStreams       = 4;
    constexpr double   kStreamRingSecs   = 0.5;

    struct Ps2Stream
    {
        bool            inUse         = false;
        bool            paused        = false;
        uint32_t        srcSampleRate = 0;
        uint8_t         numChannels   = 1;
        uint8_t         bitsPerSample = 16;
        int16_t*        ring          = nullptr;
        uint32_t        ringFrames    = 0;
        uint64_t        writeFrameAbs = 0;
        double          readFrameAbs  = 0.0;
        double          rate          = 1.0;
        int32_t         leftVolQ15    = 32768;
        int32_t         rightVolQ15   = 32768;
    };
    static Ps2Stream sStreams[kMaxStreams];

    static int      sVoiceLock    = -1;
    static int      sMixerThread  = -1;
    static volatile int32_t sLastMixPeak = 0;
    static u8       sMixerStack[16 * 1024] __attribute__((aligned(16)));
    static volatile bool sMixerRun    = false;
    static bool          sAudsrvUp    = false;

    static int16_t  sOutBuffer[kSamplesPerBuffer] __attribute__((aligned(64)));
    static int32_t  sMixBuffer[kSamplesPerBuffer] __attribute__((aligned(64)));

    inline int32_t SaturateS16(int32_t v)
    {
        if (v >  32767) return  32767;
        if (v < -32768) return -32768;
        return v;
    }

    inline void FetchFrame(const Ps2Voice& v, uint32_t srcFrame,
                           int32_t& outL, int32_t& outR)
    {
        if (v.bitsPerSample == 16)
        {
            const int16_t* s = reinterpret_cast<const int16_t*>(v.pcmData);
            if (v.numChannels == 2)
            {
                outL = s[srcFrame * 2 + 0];
                outR = s[srcFrame * 2 + 1];
            }
            else
            {
                const int32_t mono = s[srcFrame];
                outL = outR = mono;
            }
        }
        else   // 8-bit unsigned
        {
            if (v.numChannels == 2)
            {
                outL = (int32_t(v.pcmData[srcFrame * 2 + 0]) - 128) << 8;
                outR = (int32_t(v.pcmData[srcFrame * 2 + 1]) - 128) << 8;
            }
            else
            {
                const int32_t mono = (int32_t(v.pcmData[srcFrame]) - 128) << 8;
                outL = outR = mono;
            }
        }
    }

    inline void FetchStreamFrame(const Ps2Stream& s, uint64_t srcFrameAbs,
                                 int32_t& outL, int32_t& outR)
    {
        const uint32_t ringIdx = (uint32_t)(srcFrameAbs % s.ringFrames);
        if (s.numChannels == 2)
        {
            outL = s.ring[ringIdx * 2 + 0];
            outR = s.ring[ringIdx * 2 + 1];
        }
        else
        {
            const int32_t mono = s.ring[ringIdx];
            outL = outR = mono;
        }
    }

    void MixOneBuffer()
    {
        memset(sMixBuffer, 0, sizeof(sMixBuffer));

        // CRITICAL: read voice 0 state UNDER the lock (this guarantees
        // a fresh load from memory — the compiler can't cache through
        // the WaitSema/SignalSema fence).
        WaitSema(sVoiceLock);
        const bool voice0Active   = sVoices[0].active;
        const bool voice0Advanced = sVoices[0].positionFrac > 1.0;
        const uint32_t voice0Pos  = (uint32_t)sVoices[0].positionFrac;
        // Don't release lock yet — voice mixing also needs it. Inline the
        // buzz first (no shared state), then run the voice mixing below
        // still under the same lock acquisition.

        // AUDIBLE DIAGNOSTIC — buzz pitch encodes voice state:
        //   220 Hz constant hum         → voice 0 INACTIVE the whole time
        //   880 Hz constant tone        → voice 0 active, positionFrac stuck at 0
        //   Sliding pitch 220→2200 Hz   → voice 0 active AND positionFrac
        //                                 advancing through PCM (good!)
        // The sliding tone proves the mixer is processing samples.
        int buzzHz = 220;
        if (voice0Active && voice0Advanced)
        {
            // Map positionFrac (0..numFrames) to pitch (220..2200 Hz).
            // For numFrames=1.4M, voice0Pos increments by 1024 per mixer
            // iter — pitch should slide audibly over a few seconds.
            const uint32_t maxPos = sVoices[0].numFrames > 0
                                      ? sVoices[0].numFrames : 1u;
            const uint32_t ratioK = (voice0Pos * 1024u) / maxPos;  // 0..1024
            buzzHz = 220 + (int)((ratioK * 1980u) / 1024u);        // 220..2200
        }
        else if (voice0Active)
        {
            buzzHz = 880;
        }
        const int phasePeriod = kOutputRate / buzzHz / 2;
        static uint32_t sBuzzPhase = 0;
        for (int f = 0; f < kFramesPerBuffer; ++f)
        {
            const int32_t squareSample = ((sBuzzPhase / phasePeriod) & 1) ? 8000 : -8000;
            sMixBuffer[f * 2 + 0] += squareSample;
            sMixBuffer[f * 2 + 1] += squareSample;
            ++sBuzzPhase;
        }

        // Use scr_printf for one-shot diagnostic so it goes straight to
        // PCSX2's IOP console (which we can see in the user's screenshot)
        // without the host: file buffering delay. Fire on every 30th iter.
        static int sScrTick = 0;
        if (++sScrTick >= 30)
        {
            sScrTick = 0;
            scr_printf("[MX] v0a=%d pos=%u peak=%ld\n",
                       (int)voice0Active, voice0Pos, (long)sLastMixPeak);
        }

        for (uint32_t vi = 0; vi < AUDIO_MAX_VOICES; ++vi)
        {
            Ps2Voice& v = sVoices[vi];
            if (!v.active || v.pcmData == nullptr || v.numFrames == 0) continue;

            for (int f = 0; f < kFramesPerBuffer; ++f)
            {
                uint32_t srcFrame = (uint32_t)v.positionFrac;
                if (srcFrame >= v.numFrames)
                {
                    if (v.loop)
                    {
                        v.positionFrac -= (double)v.numFrames;
                        srcFrame = (uint32_t)v.positionFrac;
                        if (srcFrame >= v.numFrames) srcFrame = 0;
                    }
                    else
                    {
                        v.active = false;
                        break;
                    }
                }
                int32_t srcL, srcR;
                FetchFrame(v, srcFrame, srcL, srcR);
                sMixBuffer[f * 2 + 0] += (srcL * v.leftVolQ15)  >> 15;
                sMixBuffer[f * 2 + 1] += (srcR * v.rightVolQ15) >> 15;
                v.positionFrac += v.rate;
            }
        }

        for (uint32_t si = 0; si < kMaxStreams; ++si)
        {
            Ps2Stream& s = sStreams[si];
            if (!s.inUse || s.paused || s.ring == nullptr) continue;

            for (int f = 0; f < kFramesPerBuffer; ++f)
            {
                const uint64_t srcAbs = (uint64_t)s.readFrameAbs;
                if (srcAbs >= s.writeFrameAbs) break;   // under-run, output silence
                int32_t srcL, srcR;
                FetchStreamFrame(s, srcAbs, srcL, srcR);
                sMixBuffer[f * 2 + 0] += (srcL * s.leftVolQ15)  >> 15;
                sMixBuffer[f * 2 + 1] += (srcR * s.rightVolQ15) >> 15;
                s.readFrameAbs += s.rate;
            }
        }

        SignalSema(sVoiceLock);

        // Track peak amplitude of the post-mix buffer so the heartbeat
        // can show whether voices are actually contributing. With the
        // 220 Hz buzz baseline, peak should sit at ~10000 when no voice
        // is playing. If BossMain mixes in, peak should jump to ~31000+
        // (its peak amplitude). If voices are silently failing to mix,
        // peak stays at ~10000.
        int32_t peakSample = 0;
        for (int i = 0; i < kSamplesPerBuffer; ++i)
        {
            const int32_t mag = sMixBuffer[i] < 0 ? -sMixBuffer[i] : sMixBuffer[i];
            if (mag > peakSample) peakSample = mag;
            sOutBuffer[i] = (int16_t)SaturateS16(sMixBuffer[i]);
        }
        sLastMixPeak = peakSample;
    }

    void MixerThread(void* /*arg*/)
    {
        LogDebug("[PS2 AUD] MixerThread entered");
        // Heartbeat counter so we can verify the mixer thread is actually
        // running + submitting buffers to audsrv.
        int hb = 0;
        int verboseLeft = 5;   // log first 5 iterations in full detail
        while (sMixerRun)
        {
            if (verboseLeft > 0)
            {
                LogDebug("[PS2 AUD] mixer iter: calling wait_audio(%d)", kBytesPerBuffer);
            }
            const int waitRet = audsrv_wait_audio(kBytesPerBuffer);
            if (verboseLeft > 0)
            {
                LogDebug("[PS2 AUD] wait_audio returned %d", waitRet);
            }
            MixOneBuffer();
            const int playRet = audsrv_play_audio(reinterpret_cast<char*>(sOutBuffer), kBytesPerBuffer);
            if (verboseLeft > 0)
            {
                LogDebug("[PS2 AUD] play_audio returned %d (wanted %d)", playRet, kBytesPerBuffer);
                verboseLeft--;
            }
            if (++hb >= 30)   // ~0.7 sec @ 23ms/buf
            {
                hb = 0;
                int activeVoices = 0;
                int activeStreams = 0;
                for (int i = 0; i < AUDIO_MAX_VOICES; ++i)
                {
                    if (sVoices[i].active) ++activeVoices;
                }
                for (uint32_t i = 0; i < kMaxStreams; ++i)
                {
                    if (sStreams[i].inUse) ++activeStreams;
                }
                // Also dump voice 0's runtime state so we can see if it's
                // active, where its playhead is, and what its volume is at.
                const Ps2Voice& v0 = sVoices[0];
                LogDebug("[PS2 AUD] mixer alive: %d voices, %d streams, lastPeak=%ld | v0: active=%d pos=%u/%u volQ15=%d/%d bps=%d ch=%d rate=%.3f pcm=%p",
                         activeVoices, activeStreams, (long)sLastMixPeak,
                         (int)v0.active,
                         (unsigned)v0.positionFrac, v0.numFrames,
                         v0.leftVolQ15, v0.rightVolQ15,
                         v0.bitsPerSample, v0.numChannels,
                         v0.rate, v0.pcmData);
            }
        }

        // Drain — submit one silent buffer so we don't loop the last sample.
        memset(sOutBuffer, 0, sizeof(sOutBuffer));
        audsrv_play_audio(reinterpret_cast<char*>(sOutBuffer), kBytesPerBuffer);

        ExitThread();
    }

    inline int32_t ClampVolQ15(float v)
    {
        const float scaled = v * kMasterAtten * 32768.0f;
        if (scaled < 0.0f)     return 0;
        if (scaled > 32768.0f) return 32768;
        return (int32_t)scaled;
    }
}  // namespace

void AUD_Initialize()
{
    // Main_PS2.cpp loads + inits audsrv.irx before GameMain. If that
    // failed (no IRX, no IOP RPC, etc.), audsrv_init was never called and
    // audsrv_set_format below would crash the EE. Bail gracefully —
    // engine continues with silent audio.
    audsrv_fmt_t fmt = {};
    fmt.freq     = kOutputRate;
    fmt.bits     = 16;
    fmt.channels = 2;
    const int setFmtRet = audsrv_set_format(&fmt);
    if (setFmtRet != 0)
    {
        LogError("Audio_PS2: audsrv_set_format failed: %d (%s) — audio offline",
                 setFmtRet, audsrv_get_error_string());
        return;
    }
    sAudsrvUp = true;
    LogDebug("Audio_PS2: audsrv_set_format OK (%d Hz, 16-bit, stereo)", kOutputRate);

    const int setVolRet = audsrv_set_volume(MAX_VOLUME);
    LogDebug("Audio_PS2: audsrv_set_volume(%d) = %d (%s)",
             MAX_VOLUME, setVolRet,
             setVolRet == 0 ? "ok" : audsrv_get_error_string());

    // Startup test tone removed — proved the chain works, now it's just
    // interfering with the real mixer's wait_audio (audsrv apparently
    // doesn't auto-restart SPU2 after the ring drains, leaving wait_audio
    // blocked forever once the tone finishes).

    ee_sema_t sema = {};
    sema.init_count = 1;
    sema.max_count  = 1;
    sema.option     = 0;
    sVoiceLock = CreateSema(&sema);
    if (sVoiceLock < 0)
    {
        LogError("Audio_PS2: CreateSema failed (%d) — audio offline", sVoiceLock);
        sAudsrvUp = false;
        return;
    }

    // Capture the caller's GP register so the mixer thread can address
    // its globals correctly. Using &_gp from a different TU can be wrong
    // on PS2 EE when each .o has its own GP — grabbing $gp at runtime
    // gives the correct value for whatever the calling thread is using.
    void* currentGp = nullptr;
    asm volatile("move %0, $gp" : "=r"(currentGp));

    ee_thread_t th = {};
    th.func        = (void*)MixerThread;
    th.stack       = sMixerStack;
    th.stack_size  = sizeof(sMixerStack);
    th.gp_reg      = currentGp;
    th.initial_priority = 0x40;
    th.attr        = 0;
    th.option      = 0;
    sMixerThread = CreateThread(&th);
    if (sMixerThread < 0)
    {
        LogError("Audio_PS2: CreateThread failed (%d)", sMixerThread);
        DeleteSema(sVoiceLock);
        sVoiceLock = -1;
        sAudsrvUp = false;
        return;
    }
    sMixerRun = true;
    const int startRet = StartThread(sMixerThread, nullptr);
    LogDebug("Audio_PS2: CreateThread=%d StartThread=%d gp=%p stack=%p+%u",
             sMixerThread, startRet, currentGp, sMixerStack, (unsigned)sizeof(sMixerStack));

    LogDebug("Audio_PS2: mixer up, %d Hz stereo, %d frames/buf, %d voices, %u streams",
             kOutputRate, kFramesPerBuffer, AUDIO_MAX_VOICES, kMaxStreams);
}

void AUD_Shutdown()
{
    sMixerRun = false;
    if (sMixerThread >= 0)
    {
        // Mixer will exit on its own after one more audsrv_play_audio
        // blocking call returns. WaitSema-style wait isn't necessary —
        // PS2 EE doesn't have sceKernelWaitThreadEnd; the thread frees
        // its resources via ExitThread.
        sMixerThread = -1;
    }

    for (uint32_t i = 0; i < kMaxStreams; ++i)
    {
        if (sStreams[i].ring != nullptr) free(sStreams[i].ring);
        sStreams[i] = Ps2Stream{};
    }

    if (sVoiceLock >= 0)
    {
        DeleteSema(sVoiceLock);
        sVoiceLock = -1;
    }

    if (sAudsrvUp)
    {
        audsrv_quit();
        sAudsrvUp = false;
    }
}

void AUD_Update() {}

void AUD_Play(uint32_t voiceIndex, SoundWave* soundWave, float volume,
              float pitch, bool loop, float /*startTime*/, bool /*spatial*/)
{
    if (voiceIndex >= AUDIO_MAX_VOICES || soundWave == nullptr) return;
    if (sVoiceLock < 0) return;
    const uint8_t* pcm = soundWave->GetWaveData();
    const uint32_t numFrames = soundWave->GetNumSamples();
    if (pcm == nullptr || numFrames == 0) return;

    LogDebug("[PS2 AUD] AUD_Play voice=%u name='%s' frames=%u rate=%u ch=%u bps=%u vol=%.2f pitch=%.2f loop=%d",
             voiceIndex, soundWave->GetName().c_str(), numFrames,
             soundWave->GetSampleRate(), soundWave->GetNumChannels(),
             soundWave->GetBitsPerSample(), volume, pitch, (int)loop);
    // Dump bytes at 4 offsets across the buffer. Silence = 0x80 in 8-bit
    // unsigned PCM. If ALL offsets are 0x80 the buffer is empty silence;
    // if ONLY the start is 0x80 the audio just has an intro fade-in; if
    // mid+end vary the data is real and the silence is just the lead-in.
    auto dump8 = [&](uint32_t off) {
        if (off + 8 > numFrames) return;
        LogDebug("[PS2 AUD]   pcm[%u..%u]= %02X %02X %02X %02X  %02X %02X %02X %02X",
                 off, off+7,
                 pcm[off+0], pcm[off+1], pcm[off+2], pcm[off+3],
                 pcm[off+4], pcm[off+5], pcm[off+6], pcm[off+7]);
    };
    dump8(0);
    dump8(numFrames / 8);             // ~12% through
    dump8(numFrames / 2);             // 50%
    dump8(numFrames - 16);            // near end
    // Quick non-silence scan: count bytes in first 4 KB that differ from
    // the 0x80 midpoint. If 0, the buffer is solid silence.
    {
        uint32_t scanLen = numFrames < 4096u ? numFrames : 4096u;
        uint32_t nonSilent = 0;
        for (uint32_t i = 0; i < scanLen; ++i)
        {
            if (pcm[i] != 0x80) ++nonSilent;
        }
        LogDebug("[PS2 AUD]   non-silent bytes in first %u: %u",
                 scanLen, nonSilent);
    }

    WaitSema(sVoiceLock);
    Ps2Voice& v       = sVoices[voiceIndex];
    v.pcmData         = pcm;
    v.numFrames       = numFrames;
    v.sampleRate      = soundWave->GetSampleRate();
    v.numChannels     = (uint8_t)soundWave->GetNumChannels();
    v.bitsPerSample   = (uint8_t)soundWave->GetBitsPerSample();
    v.loop            = loop;
    v.positionFrac    = 0.0;
    v.rate            = (v.sampleRate > 0)
                          ? ((double)v.sampleRate * (double)pitch / (double)kOutputRate)
                          : 1.0;
    const int32_t vq15 = ClampVolQ15(volume);
    v.leftVolQ15      = vq15;
    v.rightVolQ15     = vq15;
    v.active          = true;
    SignalSema(sVoiceLock);
    LogDebug("[PS2 AUD] AUD_Play DONE voice=%u active-now=%d pcm=%p numFrames=%u",
             voiceIndex, (int)sVoices[voiceIndex].active,
             sVoices[voiceIndex].pcmData, sVoices[voiceIndex].numFrames);
}

void AUD_Stop(uint32_t voiceIndex)
{
    if (voiceIndex >= AUDIO_MAX_VOICES || sVoiceLock < 0) return;
    static int sStopLog = 0;
    if (sStopLog < 20)
    {
        LogDebug("[PS2 AUD] AUD_Stop voice=%u (was active=%d)",
                 voiceIndex, (int)sVoices[voiceIndex].active);
        ++sStopLog;
    }
    WaitSema(sVoiceLock);
    sVoices[voiceIndex].active = false;
    SignalSema(sVoiceLock);
}

bool AUD_IsPlaying(uint32_t voiceIndex)
{
    if (voiceIndex >= AUDIO_MAX_VOICES) return false;
    return sVoices[voiceIndex].active;
}

void AUD_SetVolume(uint32_t voiceIndex, float leftVolume, float rightVolume)
{
    if (voiceIndex >= AUDIO_MAX_VOICES || sVoiceLock < 0) return;
    // Log every SetVolume — if the engine's spatial-audio path is
    // computing 0/0 here (listener too far from source, etc.), the voice
    // goes silent without us seeing it in AUD_Play.
    static int sSetVolLog = 0;
    if (sSetVolLog < 20)
    {
        LogDebug("[PS2 AUD] AUD_SetVolume voice=%u L=%.3f R=%.3f",
                 voiceIndex, leftVolume, rightVolume);
        ++sSetVolLog;
    }
    WaitSema(sVoiceLock);
    sVoices[voiceIndex].leftVolQ15  = ClampVolQ15(leftVolume);
    sVoices[voiceIndex].rightVolQ15 = ClampVolQ15(rightVolume);
    SignalSema(sVoiceLock);
}

void AUD_SetPitch(uint32_t voiceIndex, float pitch)
{
    if (voiceIndex >= AUDIO_MAX_VOICES || sVoiceLock < 0) return;
    WaitSema(sVoiceLock);
    Ps2Voice& v = sVoices[voiceIndex];
    v.rate = (v.sampleRate > 0)
               ? ((double)v.sampleRate * (double)pitch / (double)kOutputRate)
               : 1.0;
    SignalSema(sVoiceLock);
}

uint8_t* AUD_AllocWaveBuffer(uint32_t size) { return (uint8_t*)malloc(size); }
void AUD_FreeWaveBuffer(void* buffer) { free(buffer); }
void AUD_ProcessWaveBuffer(SoundWave* /*soundWave*/) {}

uint32_t AUD_OpenStream(uint32_t sampleRate, uint32_t numChannels, uint32_t bitsPerSample)
{
    LogDebug("[PS2 AUD] AUD_OpenStream rate=%u ch=%u bps=%u",
             sampleRate, numChannels, bitsPerSample);
    if (sVoiceLock < 0) return 0;
    if (numChannels != 1 && numChannels != 2)
    {
        LogWarning("AUD_OpenStream: only mono/stereo supported (got %u)", numChannels);
        return 0;
    }
    if (bitsPerSample != 16)
    {
        LogWarning("AUD_OpenStream: only 16-bit PCM supported (got %u bps)", bitsPerSample);
        return 0;
    }

    WaitSema(sVoiceLock);
    uint32_t pickedIdx = kMaxStreams;
    for (uint32_t i = 0; i < kMaxStreams; ++i)
    {
        if (!sStreams[i].inUse) { pickedIdx = i; break; }
    }
    if (pickedIdx == kMaxStreams)
    {
        SignalSema(sVoiceLock);
        LogWarning("AUD_OpenStream: no free stream slots");
        return 0;
    }

    Ps2Stream& s = sStreams[pickedIdx];
    const uint32_t ringFrames = (uint32_t)((double)sampleRate * kStreamRingSecs);
    const uint32_t int16Count = ringFrames * numChannels;
    s.ring = (int16_t*)memalign(16, int16Count * sizeof(int16_t));
    if (s.ring == nullptr)
    {
        SignalSema(sVoiceLock);
        LogError("AUD_OpenStream: ring alloc failed");
        return 0;
    }

    s.ringFrames    = ringFrames;
    s.srcSampleRate = sampleRate;
    s.numChannels   = (uint8_t)numChannels;
    s.bitsPerSample = (uint8_t)bitsPerSample;
    s.writeFrameAbs = 0;
    s.readFrameAbs  = 0.0;
    s.rate          = (double)sampleRate / (double)kOutputRate;
    s.leftVolQ15    = ClampVolQ15(1.0f);
    s.rightVolQ15   = ClampVolQ15(1.0f);
    s.paused        = false;
    s.inUse         = true;

    SignalSema(sVoiceLock);
    return pickedIdx + 1;
}

void AUD_CloseStream(uint32_t streamId)
{
    if (streamId == 0 || streamId > kMaxStreams || sVoiceLock < 0) return;
    WaitSema(sVoiceLock);
    Ps2Stream& s = sStreams[streamId - 1];
    if (s.inUse)
    {
        if (s.ring != nullptr) { free(s.ring); s.ring = nullptr; }
        s.ringFrames    = 0;
        s.writeFrameAbs = 0;
        s.readFrameAbs  = 0.0;
        s.inUse         = false;
        s.paused        = false;
    }
    SignalSema(sVoiceLock);
}

int32_t AUD_SubmitStreamBuffer(uint32_t streamId, const uint8_t* data, uint32_t byteSize)
{
    if (streamId == 0 || streamId > kMaxStreams || sVoiceLock < 0) return 0;
    if (data == nullptr || byteSize == 0) return 0;

    WaitSema(sVoiceLock);
    Ps2Stream& s = sStreams[streamId - 1];
    if (!s.inUse || s.ring == nullptr)
    {
        SignalSema(sVoiceLock);
        return 0;
    }

    const uint32_t bpf = s.numChannels * 2;
    uint32_t submitFrames = byteSize / bpf;
    if (submitFrames == 0) { SignalSema(sVoiceLock); return 0; }
    if (submitFrames > s.ringFrames) submitFrames = s.ringFrames;

    const uint64_t readAbsU64 = (uint64_t)s.readFrameAbs;
    const uint64_t inFlight   = s.writeFrameAbs - readAbsU64;
    const uint32_t freeFrames = (inFlight < s.ringFrames)
                                  ? (uint32_t)(s.ringFrames - inFlight)
                                  : 0u;
    // Reject if no room — caller's retry path holds the chunk for next
    // tick. Memory: project_psp_stream_overflow_speedup explains the
    // alternative (fast-forwarding readFrameAbs) is broken under A/V sync
    // because video player uses audio clock as master.
    if (submitFrames > freeFrames)
    {
        SignalSema(sVoiceLock);
        return 0;
    }

    const int16_t* srcInt16 = reinterpret_cast<const int16_t*>(data);
    const uint32_t headIdx  = (uint32_t)(s.writeFrameAbs % s.ringFrames);
    const uint32_t firstChunkFrames = (headIdx + submitFrames <= s.ringFrames)
                                        ? submitFrames
                                        : (s.ringFrames - headIdx);
    memcpy(s.ring + headIdx * s.numChannels,
           srcInt16,
           firstChunkFrames * s.numChannels * sizeof(int16_t));
    if (firstChunkFrames < submitFrames)
    {
        const uint32_t wrappedFrames = submitFrames - firstChunkFrames;
        memcpy(s.ring,
               srcInt16 + firstChunkFrames * s.numChannels,
               wrappedFrames * s.numChannels * sizeof(int16_t));
    }

    s.writeFrameAbs += submitFrames;
    SignalSema(sVoiceLock);
    return (int32_t)(submitFrames * bpf);
}

uint64_t AUD_GetStreamPlayedSamples(uint32_t streamId)
{
    if (streamId == 0 || streamId > kMaxStreams) return 0;
    const Ps2Stream& s = sStreams[streamId - 1];
    return s.inUse ? (uint64_t)s.readFrameAbs : 0;
}

void AUD_SetStreamVolume(uint32_t streamId, float volume)
{
    if (streamId == 0 || streamId > kMaxStreams || sVoiceLock < 0) return;
    WaitSema(sVoiceLock);
    Ps2Stream& s = sStreams[streamId - 1];
    if (s.inUse)
    {
        s.leftVolQ15  = ClampVolQ15(volume);
        s.rightVolQ15 = ClampVolQ15(volume);
    }
    SignalSema(sVoiceLock);
}

void AUD_SetStreamPaused(uint32_t streamId, bool paused)
{
    if (streamId == 0 || streamId > kMaxStreams || sVoiceLock < 0) return;
    WaitSema(sVoiceLock);
    if (sStreams[streamId - 1].inUse) sStreams[streamId - 1].paused = paused;
    SignalSema(sVoiceLock);
}

void AUD_FlushStream(uint32_t streamId)
{
    if (streamId == 0 || streamId > kMaxStreams || sVoiceLock < 0) return;
    WaitSema(sVoiceLock);
    Ps2Stream& s = sStreams[streamId - 1];
    if (s.inUse)
    {
        s.writeFrameAbs = (uint64_t)s.readFrameAbs;
    }
    SignalSema(sVoiceLock);
}

#endif // POLYPHASE_PLATFORM_ADDON
