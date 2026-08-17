#ifdef TARGET_OGC

#include <ogc/lwp_watchdog.h>   // gettime, ticks_to_microsecs

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../configfile.h"
#include "../storage_ogc.h"
#include "gfx_perf.h"

// One entry per second of play. 2048 of them is 34 minutes, which covers the
// 30-minute session STORY-019 asks for; past that the oldest are overwritten
// and the log says so, because a silently truncated measurement is worse than a
// short one.
#define GFX_PERF_SECONDS 2048

#define GFX_PERF_LOG_NAME "perf.log"

struct GfxPerfSecond {
    uint32_t frames;
    uint32_t frame_us_sum;
    uint32_t frame_us_max;
    uint32_t bucket_us_sum[GFX_PERF_NUM_BUCKETS];
    uint32_t bucket_us_max[GFX_PERF_NUM_BUCKETS];
};

static bool perf_on;
static bool perf_written;
static struct GfxPerfSecond *perf_ring;
static uint32_t perf_seconds;      // seconds recorded, may exceed the ring
static uint64_t perf_second_start; // ticks at the start of the current second

// The frame being measured. Buckets accumulate within it and are folded into
// the current second at frame_end.
static uint64_t perf_frame_start;
static uint32_t perf_frame_bucket_us[GFX_PERF_NUM_BUCKETS];

void gfx_perf_init(void) {
    if (!configPerfLog) {
        return;
    }

    perf_ring = calloc(GFX_PERF_SECONDS, sizeof(*perf_ring));
    if (perf_ring == NULL) {
        return;   // out of memory is not a reason to stop the game
    }

    perf_on = true;
    perf_second_start = gettime();
}

bool gfx_perf_enabled(void) {
    return perf_on;
}

uint64_t gfx_perf_now(void) {
    return gettime();
}

void gfx_perf_account(enum GfxPerfBucket bucket, uint64_t started) {
    if (!perf_on || bucket >= GFX_PERF_NUM_BUCKETS) {
        return;
    }
    perf_frame_bucket_us[bucket] += (uint32_t) ticks_to_microsecs(gettime() - started);
}

void gfx_perf_frame_begin(void) {
    if (!perf_on) {
        return;
    }
    perf_frame_start = gettime();
    memset(perf_frame_bucket_us, 0, sizeof(perf_frame_bucket_us));
}

void gfx_perf_frame_end(void) {
    if (!perf_on) {
        return;
    }

    const uint64_t now = gettime();
    const uint32_t frame_us = (uint32_t) ticks_to_microsecs(now - perf_frame_start);

    struct GfxPerfSecond *s = &perf_ring[perf_seconds % GFX_PERF_SECONDS];
    s->frames++;
    s->frame_us_sum += frame_us;
    if (frame_us > s->frame_us_max) {
        s->frame_us_max = frame_us;
    }
    for (int i = 0; i < GFX_PERF_NUM_BUCKETS; i++) {
        s->bucket_us_sum[i] += perf_frame_bucket_us[i];
        if (perf_frame_bucket_us[i] > s->bucket_us_max[i]) {
            s->bucket_us_max[i] = perf_frame_bucket_us[i];
        }
    }

    // Roll over to the next second. Comparing in microseconds rather than ticks
    // keeps this readable and the arithmetic is done once a frame.
    if (ticks_to_microsecs(now - perf_second_start) >= 1000000u) {
        perf_second_start = now;
        perf_seconds++;
        memset(&perf_ring[perf_seconds % GFX_PERF_SECONDS], 0, sizeof(*perf_ring));
    }
}

void gfx_perf_write_log(void) {
    if (!perf_on || perf_written || perf_ring == NULL) {
        return;
    }
    perf_written = true;

    FILE *f = fopen(storage_ogc_path(GFX_PERF_LOG_NAME), "w");
    if (f == NULL) {
        return;
    }

    // Every column is microseconds, averaged over the frames of that second,
    // except the *_max columns which are the worst single frame in it. The max
    // columns are the ones that answer "no hitch longer than 100 ms".
    //
    // cpu is what is left after the two waits: the work the CPU actually did.
    // If vsync is large the console has headroom; if gp_wait is large the GP is
    // the bottleneck and no amount of CPU optimisation will help.
    fprintf(f, "# sm64 perf log -- STORY-018 task 1\n");
    fprintf(f, "# one line per second of play, microseconds unless noted\n");
    fprintf(f, "# sec frames fps frame_avg frame_max cpu_avg submit_avg submit_max "
               "gp_wait_avg gp_wait_max vsync_avg vsync_max\n");

    const uint32_t total = perf_seconds + 1;
    uint32_t first = 0;
    if (total > GFX_PERF_SECONDS) {
        first = total - GFX_PERF_SECONDS;
        fprintf(f, "# ring wrapped: the first %u seconds were overwritten\n", first);
    }

    for (uint32_t sec = first; sec < total; sec++) {
        const struct GfxPerfSecond *s = &perf_ring[sec % GFX_PERF_SECONDS];
        if (s->frames == 0) {
            continue;
        }
        const uint32_t n = s->frames;
        const uint32_t frame_avg = s->frame_us_sum / n;
        const uint32_t submit_avg = s->bucket_us_sum[GFX_PERF_SUBMIT] / n;
        const uint32_t gp_avg = s->bucket_us_sum[GFX_PERF_GP_WAIT] / n;
        const uint32_t vsync_avg = s->bucket_us_sum[GFX_PERF_VSYNC_WAIT] / n;
        const uint32_t waits = gp_avg + vsync_avg;
        const uint32_t cpu_avg = (frame_avg > waits) ? (frame_avg - waits) : 0;

        fprintf(f, "%u %u %u %u %u %u %u %u %u %u %u %u\n",
                sec, n, n,
                frame_avg, s->frame_us_max,
                cpu_avg,
                submit_avg, s->bucket_us_max[GFX_PERF_SUBMIT],
                gp_avg, s->bucket_us_max[GFX_PERF_GP_WAIT],
                vsync_avg, s->bucket_us_max[GFX_PERF_VSYNC_WAIT]);
    }

    fclose(f);
}

#endif // TARGET_OGC
