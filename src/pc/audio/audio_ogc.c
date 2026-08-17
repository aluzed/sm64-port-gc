#ifdef TARGET_OGC

// Audio backend for GameCube and Wii, driving the audio interface directly.
//
// Three things line up unusually well here and are worth stating, because they
// are why this backend is short and why nothing is resampled or byte-swapped:
//
//  * SM64's mixer produces signed 16-bit stereo at ~32 kHz, and the AI supports
//    32 kHz natively.
//  * PowerPC is big-endian and so is the AI, so the samples go out untouched.
//  * pc_main.c hands us 2 * num_samples * 4 bytes, already a multiple of the 32
//    the DMA needs.
//
// libogc's AESND/ASND mixers were not used: they run at 48 kHz and would resample
// a stream that is already mixed and already at the right rate.
//
// Only the narrow libogc headers are included. <gccore.h> would drag in ogc/gx.h,
// whose Vtx and Mtx clash with PR/gbi.h -- see docs/stories/003.

#include <ogc/audio.h>
#include <ogc/cache.h>
#include <ogc/machine/processor.h>

#include <string.h>

#include "audio_api.h"
#include "audio_ogc.h"

// Four buffers is about 135 ms of slack: enough to ride out a long game frame
// without adding latency anyone would notice.
#define NUM_BUFFERS 4

// Worst case is the EU version's 656 samples: 656 * 2 blocks * 2 channels * 2
// bytes = 5248, rounded up to a multiple of 32.
#define BUFFER_SIZE 5248

#define BYTES_PER_SAMPLE 4   // stereo, 16-bit

static u8 buffers[NUM_BUFFERS][BUFFER_SIZE] ATTRIBUTE_ALIGN(32);
static u32 buffer_len[NUM_BUFFERS];

// Shared with the DMA interrupt; every access is inside an ISR-disabled section.
static volatile int read_idx;
static volatile int write_idx;
static volatile int queued;
static volatile bool dma_running;

static void audio_ogc_start_locked(void) {
    if (queued == 0) {
        dma_running = false;
        return;
    }
    AUDIO_InitDMA((u32) buffers[read_idx], buffer_len[read_idx]);
    AUDIO_StartDMA();
    read_idx = (read_idx + 1) % NUM_BUFFERS;
    queued--;
    dma_running = true;
}

// Runs in interrupt context at the end of each transfer.
static void audio_ogc_dma_callback(void) {
    audio_ogc_start_locked();
}

void audio_ogc_stop(void) {
    // Unregister before stopping, not after. The callback re-arms the engine
    // from whatever is still queued, so stopping first leaves a window in which
    // the interrupt starts another transfer and the stop is undone.
    u32 level;
    _CPU_ISR_Disable(level);
    queued = 0;
    dma_running = false;
    _CPU_ISR_Restore(level);

    AUDIO_RegisterDMACallback(NULL);
    AUDIO_StopDMA();
}

static bool audio_ogc_init(void) {
    read_idx = 0;
    write_idx = 0;
    queued = 0;
    dma_running = false;
    memset(buffers, 0, sizeof(buffers));
    DCFlushRange(buffers, sizeof(buffers));

    AUDIO_Init(NULL);
    AUDIO_SetDSPSampleRate(AI_SAMPLERATE_32KHZ);
    AUDIO_RegisterDMACallback(audio_ogc_dma_callback);
    return true;
}

static int audio_ogc_buffered(void) {
    u32 level;
    _CPU_ISR_Disable(level);
    const int q = queued;
    _CPU_ISR_Restore(level);

    // Samples still queued, plus the one the DMA is currently playing. Slight
    // over-estimate, which is the safe direction: it makes pc_main.c produce
    // the shorter block and lets the queue drain.
    return (q + (dma_running ? 1 : 0)) * (BUFFER_SIZE / BYTES_PER_SAMPLE);
}

static int audio_ogc_get_desired_buffered(void) {
    // Same figure the PC backends use; pc_main.c compares against it to pick
    // between the short and long sample block.
    return 1100;
}

static void audio_ogc_play(const uint8_t *buf, size_t len) {
    if (len > BUFFER_SIZE) {
        len = BUFFER_SIZE;
    }
    // The DMA works in 32-byte units; pc_main.c already supplies a multiple,
    // but round down rather than hand the hardware a partial unit.
    len &= ~31u;
    if (len == 0) {
        return;
    }

    u32 level;
    _CPU_ISR_Disable(level);
    const bool full = (queued >= NUM_BUFFERS);
    const int slot = write_idx;
    if (!full) {
        write_idx = (write_idx + 1) % NUM_BUFFERS;
    }
    _CPU_ISR_Restore(level);

    if (full) {
        // Queue saturated: dropping this block keeps us in sync, whereas
        // blocking would stall the game loop.
        return;
    }

    memcpy(buffers[slot], buf, len);
    // The DSP reads main memory, not the CPU's L1. Without this the sound is
    // whatever happened to be in RAM.
    DCFlushRange(buffers[slot], len);
    buffer_len[slot] = (u32) len;

    _CPU_ISR_Disable(level);
    queued++;
    if (!dma_running) {
        audio_ogc_start_locked();
    }
    _CPU_ISR_Restore(level);
}

struct AudioAPI audio_ogc = {
    audio_ogc_init,
    audio_ogc_buffered,
    audio_ogc_get_desired_buffered,
    audio_ogc_play
};

#endif // TARGET_OGC
