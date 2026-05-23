# PS2 Audio — Polyphase Build Target

`Runtime/PS2/Audio_PS2.cpp` is a software 8-voice mixer + 4 parallel streams
feeding a single audsrv output channel. The architecture mirrors the PSP
implementation (`Audio_PSP.cpp`), with one important deviation: how the
mixer thread is paced.

## Engine API surface

The engine talks to PS2 audio through the `AUD_*` API in
`Engine/Source/Audio/Audio.h`:

| Call | What it does on PS2 |
| --- | --- |
| `AUD_Initialize()` | Sets audsrv format (44.1 kHz / 16 / stereo), creates the voice-lock semaphore, captures `$gp`, creates the mixer thread at priority 0x30. |
| `AUD_Shutdown()` | Signals the mixer to exit, polls `ReferThreadStatus` until `THS_DORMANT` (250 ms cap), tears down audsrv + libmc state. |
| `AUD_Play(voice, soundWave, vol, pitch, loop, ...)` | Under the voice-lock, populates `sVoices[voice]` with PCM pointer + resample rate; clamps pitch to `[0.01, 8.0]`. |
| `AUD_Stop / SetVolume / SetPitch / IsPlaying` | Mutate the voice entry under the lock. |
| `AUD_OpenStream / SubmitStreamBuffer / CloseStream / FlushStream / GetStreamPlayedSamples / SetStreamVolume / SetStreamPaused` | Parallel stream channels, ring-buffered at the source sample rate. |

## Voice + stream model

```
   Game thread                  Mixer thread
  ─────────────                ────────────────
                                   loop:
   AUD_Play(0) ──► sVoices[0]      audsrv_wait_audio(kBytesPerBuffer)
                       │             │
   AUD_Play(1) ──► sVoices[1]        ▼
                       │           MixOneBuffer()
                       ▼             │  ┌─ for each active voice
                  [voice table:      │  │   nearest-neighbour resample
                   8 entries]        │  │   v.positionFrac += v.rate
                       │             │  │   accumulate into sMixBuffer[]
                       ▼             │  └─ saturate to s16, post-mix limiter
                  [stream ring]      │
                       │             ▼
                       ▼          audsrv_play_audio(sOutBuffer, N)
                  Mixer reads
```

### Voice table (8 entries)

Each `Ps2Voice` is the engine's claim on one of 8 slots:

- `pcmData` — weak pointer to engine-owned PCM; engine keeps the SoundWave
  alive via asset refcount.
- `positionFrac` — fractional source-frame cursor (mixer-private, no lock
  needed once written by `AUD_Play`).
- `rate` — advance per output frame, computed as
  `sampleRate * pitch / 44100`.
- `leftVolQ15` / `rightVolQ15` — Q15 per-side volume (0..32768). Engine
  passes 0..1 float; `ClampVolQ15` multiplies by `kMasterAtten` (0.5x =
  6 dB headroom) and converts to fixed-point.
- `active` — `volatile bool` flag. See "volatile note" below.

### Stream slots (4 entries)

`Ps2Stream` is the engine's video-player / external-PCM-feeder interface:

- Caller submits PCM ahead of time via `AUD_SubmitStreamBuffer`, mixer
  drains at output rate.
- `writeFrameAbs` (monotonic uint64) and `readFrameAbs` (fractional double)
  use absolute frame counters rather than byte cursors with wrap math —
  much easier to reason about under-runs.
- Ring depth is 0.5 s of PCM at the source rate (`kStreamRingSecs`).
- A/V sync consumers (VideoPlayer) query `AUD_GetStreamPlayedSamples` —
  returns `(uint64_t)readFrameAbs`. The audio clock is the master clock
  for video.
- If submit can't fit, returns 0 bytes accepted; caller's retry path holds
  the chunk for next tick. The PSP implementation tried fast-forwarding
  `readFrameAbs` here, which broke A/V sync (video raced ahead because its
  master clock jumped). Don't reintroduce that.

## The audsrv pacing fix

`audsrv` is the canonical PS2 homebrew audio server. It's an IRX running
on the IOP, exposing EE-side stubs (`libaudsrv.a`) that RPC to it.

`audsrv.h` defines two important calls:

```c
/** Uploads audio buffer to SPU
 * ... will not interrupt a playing buffer, rather queue it up ...
 * @returns positive number of bytes sent to processor or negative error */
extern int audsrv_play_audio(const char *chunk, int bytes);

/** Blocks until there is enough space to enqueue chunk
 * Blocks until there are enough space to store the upcoming chunk
 * in audsrv's internal ring buffer.
 * @returns error code */
extern int audsrv_wait_audio(int bytes);
```

**`audsrv_play_audio` is non-blocking.** It queues into audsrv's IOP-side
ring and returns immediately. If the ring is full, it accepts fewer bytes
than requested (return value < `bytes`).

**`audsrv_wait_audio(bytes)` is the genuine blocking primitive.** It blocks
until the ring has at least `bytes` of free space.

### The bug we fixed

Earlier versions of this file paced the mixer with `usleep(20 ms)` after
each `audsrv_play_audio` call, on the assumption that `audsrv_play_audio`
itself blocks (mirroring `sceAudioOutputBlocking` on PSP, which truly does).
It doesn't, and the symptoms were:

- Audio plays exactly one buffer, then silence (audsrv ring drained during
  the 20 ms sleep; SPU2 finishes its last block; audsrv does not auto-
  restart SPU2 from drain).
- Sometimes — if mix happened fast enough that the next submit landed
  before the drain — a second buffer played. Hence "had to be triggered 6
  times to hear anything".
- Voice cursors advanced every mix call, so retriggered sounds came out at
  random fractional offsets — "sounds don't sync".

### The fix

The mixer now uses `audsrv_wait_audio` to park between submits:

```cpp
while (sMixerRun) {
    audsrv_wait_audio(kBytesPerBuffer);
    MixOneBuffer();
    audsrv_play_audio((char*)sOutBuffer, kBytesPerBuffer);
}
```

The wait is BEFORE the mix, not after the submit, so the freshly-mixed
buffer is as close as possible to the playhead (lower added latency). The
EE thread sleeps inside `audsrv_wait_audio`, so other EE threads (including
the game) run freely while we're idle.

Cap on shutdown: `AUD_Shutdown` polls `ReferThreadStatus` until the mixer
enters `THS_DORMANT` (capped at 250 ms) before deleting the semaphore the
mixer holds. Without this cap, a wedged audsrv could deadlock shutdown.

## Mix math

`MixOneBuffer` accumulates voices and streams into a stereo `int32_t`
buffer (`sMixBuffer`), then post-processes:

1. **Per-voice contribution** — fetch one source frame (mono or stereo,
   8-bit unsigned or 16-bit signed), multiply by `leftVolQ15`/`rightVolQ15`,
   right-shift by 15 (the Q15 → integer step). Accumulate into `sMixBuffer`.
2. **Per-stream contribution** — same shape, but the source is the
   stream's ring at `(uint64_t)readFrameAbs`. If `readFrameAbs >=
   writeFrameAbs`, we hit underrun and break the inner loop (rest of the
   buffer stays at 0 — silence).
3. **Post-mix limiter** — scan `sMixBuffer` for peak magnitude. If it
   exceeds s16 range, compute `gainQ15 = (32767 << 15) / peakMag` and scale
   every sample. Single voices stay full loudness; 8-voice rapid-fire SFX
   stack without hard-clipping (which sounded like rising pitch due to
   odd-harmonic stacking). Simple per-buffer normaliser — may pump on
   transients, but no hard clip. Inaudible at low voice counts (the
   common case).
4. **Saturate to s16** — `(int16_t)SaturateS16(sMixBuffer[i])` into
   `sOutBuffer`, which audsrv consumes.

## Threading and volatile

The voice-lock semaphore (`sVoiceLock`, init=1, max=1 — i.e. a mutex)
guards mutation of `sVoices` and `sStreams`. Both the engine-side and
mixer-side write paths take it.

`Ps2Voice::active` is `volatile bool`. The comment in the file documents a
real gcc-15 hoisting incident: without `volatile`, gcc hoisted
`sVoices[0].active` to a register at function entry and never reloaded
after the game thread's `AUD_Play` updated memory — so the mixer never
saw the voice become active. Adding `volatile` forces every read through
memory.

`Ps2Voice::positionFrac` is NOT volatile. It's mixer-thread-private
(written to 0.0 by `AUD_Play` once under the lock; from then on only the
mixer touches it). Marking it volatile forces every `+= rate` through
memory, and on PS2's non-IEC559 FPU that produced wonky comparisons —
voice cursors never reached `numFrames` cleanly, so one-shot sounds
looped forever.

`AUD_GetStreamPlayedSamples` reads `readFrameAbs` (a `double`, 8 bytes)
without the lock. The R5900 EE has 32-bit-aligned loads only, so this
isn't atomic in theory — a torn read would produce a momentarily-wrong
A/V sync sample (not a crash). The tradeoff is documented in the comment
above the function. If sync glitches surface, switch to a separately-
maintained 64-bit uint counter under the lock.

## Tuning knobs

| Constant | Default | Effect |
| --- | --- | --- |
| `kOutputRate` | 44100 | audsrv format frequency. Don't change without testing PCSX2 + real-HW. |
| `kFramesPerBuffer` | 1024 | One buffer = ~23.2 ms of audio. Smaller → lower latency but more `audsrv_wait_audio` cycles per second. |
| `kSamplesPerBuffer` | `kFramesPerBuffer * 2` | Derived (stereo) |
| `kBytesPerBuffer` | `kSamplesPerBuffer * 2` | Derived (s16) — argument to `audsrv_wait_audio` |
| `kMasterAtten` | 0.5 | Per-voice base attenuation. 0.5 = 6 dB headroom for 2-voice mixing; the post-mix limiter handles >2-voice clipping dynamically. |
| `kMaxStreams` | 4 | Concurrent video/streaming-PCM consumers |
| `kStreamRingSecs` | 0.5 | Per-stream ring depth in source-rate seconds |
| Mixer thread priority | 0x30 | One step above main (0x40). Lower number = higher EE priority. Safe because the mixer is parked most of its life. |
| Mixer stack | 16 KB | Plenty — mixer only uses fast-fetch + accumulate. |

## Diagnostics

If audio behaviour gets weird, audsrv exposes two introspection calls:

- `audsrv_available()` — bytes free in the ring (how much we could submit now)
- `audsrv_queued()` — bytes already in the ring (how much will play before drain)

Logging these from inside `MixerThread` answers questions like "are we
under-feeding?" (queued stays low) vs "are we over-submitting?" (available
stays at zero, calls return less than requested).

`AUD_Play` logs the first 30 play requests (`[PS2 AUD] AUD_Play v=...`)
which gives a quick "did the engine actually issue the call?" check when
SFX go missing.

## Open work

- ADPCM voice path — the SPU2 has 24 HW ADPCM voices via
  `audsrv_load_adpcm`/`audsrv_ch_play_adpcm`. Free, since they don't burn
  EE CPU. Future enhancement: cook short SFX to ADPCM and play through HW;
  reserve the software mixer for streams + Vorbis-decoded music.
- Linear-interpolation resampling — current is nearest-neighbour, which
  has audible aliasing on aggressive pitch ramps.
- 8 kHz / 22 kHz source rate support — works but the resampler operates
  at output rate, so upsampling artifacts are pronounced. ADPCM HW path
  would mitigate.

## References

- [PS2SDK audsrv source](https://github.com/ps2dev/ps2sdk/tree/master/iop/sound/audsrv) — IRX side
- [PS2SDK libaudsrv source](https://github.com/ps2dev/ps2sdk/tree/master/ee/audio/audsrv) — EE-side stubs
- [SPU2 hardware reference](https://psi-rockin.github.io/ps2tek/#spu2) — register-level SPU2 docs
