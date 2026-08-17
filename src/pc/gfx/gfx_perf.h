#ifndef GFX_PERF_H
#define GFX_PERF_H

#ifdef TARGET_OGC

#include <stdbool.h>
#include <stdint.h>

// Frame timing, for STORY-018.
//
// The story gates every optimisation on this: "no optimisation applied without
// a prior measurement". It also says Dolphin gives no useful performance signal
// -- it runs on an x86 hundreds of times faster than a Gekko -- so the numbers
// have to come off a console, during a real session, with nobody watching a
// counter. Hence a log rather than an overlay.
//
// It writes one line per second of play to the storage device, and writes it at
// shutdown, which is why the clean-exit work in STORY-019 had to come first:
// pulling the plug loses the log.
//
// Off by default and allocated lazily, so a normal build carries no cost and no
// footprint. Turn it on with `perf_log = true` in sm64config.txt.
//
// Nothing here names a libogc or an ultra64 type, so it is safe to include from
// either side of the header split described in controller_wpad.h.

// What a slice of frame time is charged to. Anything not charged to a bucket is
// CPU work, which is the number that matters most: the waits tell you who is
// the bottleneck, and the remainder tells you how much room there is.
enum GfxPerfBucket {
    GFX_PERF_SUBMIT,      // inside gfx_gx_draw_triangles: building the FIFO
    GFX_PERF_GP_WAIT,     // blocked in GX_DrawDone: the GP is behind
    GFX_PERF_VSYNC_WAIT,  // blocked in VIDEO_WaitVSync: there is headroom
    GFX_PERF_NUM_BUCKETS,
};

// Reads the config option and allocates the ring. Call once, after the config
// has been loaded and the storage device mounted.
void gfx_perf_init(void);

// True when logging is on. Exposed so callers can skip taking a timestamp at
// all rather than take one and throw it away.
bool gfx_perf_enabled(void);

// A timestamp in whatever unit the platform counts in. Cheap -- a register read.
uint64_t gfx_perf_now(void);

// Charges `gfx_perf_now() - started` to a bucket.
void gfx_perf_account(enum GfxPerfBucket bucket, uint64_t started);

// Frame boundaries, around one full game iteration.
void gfx_perf_frame_begin(void);
void gfx_perf_frame_end(void);

// Writes the log out. Called from the shutdown path; safe to call when logging
// was never enabled, and safe to call twice.
void gfx_perf_write_log(void);

#endif // TARGET_OGC

#endif
