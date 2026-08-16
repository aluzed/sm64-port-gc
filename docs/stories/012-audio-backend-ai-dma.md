# STORY-012 — 32 kHz stereo audio backend (AI DMA)

**Epic:** 3 — Audio
**Status:** To do
**Depends on:** STORY-003 *(parallelisable with the GX epic)*
**Estimate:** M (2-3 d)
**Platform:** GC + Wii

## Context

`AudioAPI` is the simplest interface in the port — four functions:

```c
bool (*init)(void);
int  (*buffered)(void);              /* samples still queued */
int  (*get_desired_buffered)(void);  /* fill target */
void (*play)(const uint8_t *buf, size_t len);
```

`pc_main.c` calls `play()` once per game frame with `2 * num_audio_samples * 4` bytes, where
`num_audio_samples` is 544 or 528 (656/640 in the EU version) depending on the fill level
reported by `buffered()`. The format SM64's mixer produces is **signed 16-bit PCM, interleaved
stereo, ~32,000 Hz, native endianness**.

Three happy coincidences with Nintendo hardware:

1. The GameCube and Wii audio interface (AI) natively supports **32 kHz** — no resampling.
2. PowerPC is big-endian and AI expects big-endian → **no conversion**.
3. The requested rate (4,352 bytes per call) is already a multiple of 32, the granularity the
   DMA requires.

Two possible implementations:

| | Direct AI DMA | AESND / ASND |
|---|---|---|
| Rate | native 32 kHz | 48 kHz, internal resampling |
| Latency | minimal | +1 mixing buffer |
| Complexity | ~150 lines | ~80 lines |
| Fidelity | exact | degraded by resampling |

**Decision: direct AI DMA.** libogc's software mixer adds nothing here — SM64 already does its
own mixing and outputs a single stereo stream.

## Goal

As a player, I want to hear the music and sound effects with no crackle, no lag and at the
right pitch, so the game is playable.

## Acceptance criteria

- [ ] The main theme plays at the right speed and pitch (compare by ear with the PC build).
- [ ] No crackle, skip or intermittent silence after 10 minutes of continuous play.
- [ ] No perceptible audio/video desync over a long session.
- [ ] Left and right channels are not swapped (test: a sound to Mario's left comes out left).
- [ ] `buffered()` returns a coherent value letting `pc_main.c` alternate correctly between
      528 and 544 samples.
- [ ] Audio stops cleanly on exit (no residual white noise).

## Tasks

1. **Create `src/pc/audio/audio_ogc.c` / `audio_ogc.h`** exporting `struct AudioAPI audio_ogc`.

2. **Ring of DMA buffers.**
   ```c
   #define NUM_BUFFERS 4
   #define BUFFER_SIZE 4608        /* >= 656*2*4 (EU), multiple of 32 */
   static u8 buffers[NUM_BUFFERS][BUFFER_SIZE] ATTRIBUTE_ALIGN(32);
   static volatile int read_idx, write_idx, queued;
   static u32 buffer_len[NUM_BUFFERS];
   ```
   Four buffers ≈ 135 ms of slack: enough to absorb a long game frame without adding annoying
   latency.

3. **Initialisation.**
   ```c
   AUDIO_Init(NULL);
   AUDIO_SetDSPSampleRate(AI_SAMPLERATE_32KHZ);
   AUDIO_RegisterDMACallback(dma_callback);
   ```

4. **DMA callback** — called from interrupt at the end of each transfer: start the next
   queued buffer, or stop the DMA when starved rather than looping on stale data.

5. **`play()`** — copy into the write buffer, flush the cache, start the DMA if idle:
   ```c
   memcpy(buffers[write_idx], buf, len);
   DCFlushRange(buffers[write_idx], len);   /* mandatory */
   ```
   `DCFlushRange` matters as much here as for textures: the DSP reads RAM, not the CPU's L1.

6. **`buffered()`** — return the number of stereo samples still queued.
   `get_desired_buffered()` → **1100** (the same value the PC backends use; it is what
   `pc_main.c` compares against to choose 528 vs 544).

7. **Concurrency safety.** `queued`, `read_idx` and `write_idx` are shared between the main
   context and the interrupt context. Protect the critical sections with
   `u32 level; _CPU_ISR_Disable(level); … _CPU_ISR_Restore(level);`. An unprotected counter
   gives rare crackles that are impossible to reproduce on demand.

8. **Wiring** in `pc_main.c`, keeping the existing `audio_null` fallback.

## Files touched

- `src/pc/audio/audio_ogc.c`, `src/pc/audio/audio_ogc.h` (new)
- `src/pc/pc_main.c`
- `Makefile`

## Notes and risks

- **Exact rate.** SM64 runs at 30 frames/s and produces 528 or 544 samples ×2 per frame, i.e.
  31,680 to 32,640 Hz — the game self-regulates through `buffered()`. If the game runs at a
  different cadence (PAL 50 Hz, see STORY-011), the audio rate drifts and starves continuously.
  **Audio and video cadence must be validated together.**
- Do not call `AUDIO_StartDMA()` from the main context while an interrupt is being handled
  without protection: that is the classic cause of a DMA "starting twice" and choppy sound.
- Dolphin is more tolerant than hardware about audio starvation. Perfect sound on the emulator
  guarantees nothing; validate on a console.
- The EU version uses 656/640 samples: size `BUFFER_SIZE` for the worst case, not for US.
