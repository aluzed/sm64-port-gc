#ifdef TARGET_OGC

// Window-manager backend for GameCube and Wii (libogc).
//
// There is no window and no event loop on a console, so most of
// GfxWindowManagerAPI collapses into no-ops. What is left is the part that
// actually matters: bringing up the video interface, allocating the external
// framebuffers, and pacing the game loop on the vertical retrace.
//
// This file also owns the GX bring-up and the EFB -> XFB copy. That was first
// done so the port could boot before gfx_gx.c existed, and it stayed: those
// settings are a function of the video mode (fbWidth, efbHeight, aa,
// sample_pattern, vfilter), not of the renderer, and keeping them here is what
// lets ENABLE_GFX_DUMMY builds still come up on screen. gfx_gx.c owns the
// render state -- TEV, textures, depth, blending, vertex submission.

#include <gccore.h>
#include <ogcsys.h>
#include <ogc/lwp_watchdog.h> // gettime, ticks_to_microsecs

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "../audio/audio_ogc.h"
#include "../configfile.h"
#include "gfx_ogc.h"
#include "gfx_pc_aspect.h"
#include "gfx_perf.h"
#include "gfx_window_manager_api.h"

#define OGC_FIFO_SIZE (256 * 1024)

static GXRModeObj *rmode;
static void *xfb[2];
static int cur_xfb;
// Set by swap_buffers_begin, i.e. only when the game submitted a display list.
static bool frame_has_content;
// The picture's aspect, 4:3 or 16:9, which the framebuffer's dimensions do not
// give away. See gfx_ogc_start_frame.
static float display_aspect = 4.0f / 3.0f;
static bool display_is_progressive;
static void *gp_fifo;

static uint64_t start_ticks;

// -- frame pacing ------------------------------------------------------------
//
// 60 Hz modes present every two retraces, which is exactly 29.97 fps and
// exactly what the N64 did. 50 Hz cannot express 30 fps that way, so it gets
// time-based pacing instead: wait whole retraces until 1/30 s has elapsed,
// averaging 30 game frames per second with a regular 2-2-1 retrace pattern.
//
// Forcing PAL60 was considered and rejected: it overrides the console's own
// setting, and on a TV that cannot do 60 Hz the result is no picture at all.
#define VSYNCS_PER_FRAME 2
#define GAME_FRAME_USEC  33333

static bool fifty_hz;
static u64 next_frame_ticks;

static bool video_is_50hz(void) {
    const u32 tv = VIDEO_GetCurrentTvMode();
    return tv == VI_PAL || tv == VI_DEBUG_PAL;
}
static volatile bool should_quit;
static volatile bool should_power_off;


// -- shutdown ---------------------------------------------------------------

void gfx_ogc_request_quit(bool power_off) {
    if (power_off) {
        should_power_off = true;
    }
    should_quit = true;
}

// These run in interrupt context: they may only raise a flag. Touching the
// filesystem or GX from here locks the console in a way that is very hard to
// diagnose.
static void on_reset_pressed(u32 irq, void *ctx) {
    (void) irq;
    (void) ctx;
    gfx_ogc_request_quit(false);
}

#ifdef TARGET_WII
// The GameCube has no soft power button, so libogc only exposes this on Wii.
static void on_power_pressed(void) {
    gfx_ogc_request_quit(true);
}
#endif

// Registered by pc_main *before* its own atexit(save_config), so that it runs
// after it: atexit handlers fire last-registered-first.
//
// The ordering is the whole point, and getting it wrong is silent. Powering off
// from gfx_ogc_shutdown directly -- which is what this used to do -- never
// reaches exit(), so the config was written on the RESET and HOME paths and
// quietly lost every time the player used the POWER button.
void gfx_ogc_finish_shutdown(void) {
#ifdef TARGET_WII
    if (should_power_off) {
        SYS_ResetSystem(SYS_POWEROFF, 0, 0);
    }
#endif
}

static void gfx_ogc_shutdown(void) {
    // Stop the engines that keep running on their own before tearing anything
    // down. There is no OS here to reclaim a device: the audio DMA would keep
    // walking its buffer, and the GP would keep consuming a FIFO whose memory
    // the next program is about to reuse.
    audio_ogc_stop();
    GX_AbortFrame();

    // Before the video goes black, while the storage device is still mounted.
    // A log that only exists if the player quits politely is the price of not
    // stalling the frame loop to write it; STORY-019's exit paths are what make
    // that acceptable.
    gfx_perf_write_log();

    VIDEO_SetBlack(TRUE);
    VIDEO_Flush();
    VIDEO_WaitVSync();

    // atexit runs save_config, then gfx_ogc_finish_shutdown. On Wii the loader
    // installs a return stub, so exit() goes back to the Homebrew Channel;
    // SYS_ResetSystem in the finisher is the fallback when it does not.
    exit(0);
}

// -- init -------------------------------------------------------------------

static void gfx_ogc_init_video(void) {
    VIDEO_Init();

    // Honours the console's own settings: PAL/NTSC and 50/60 Hz. Never override
    // them -- forcing PAL60 on a console set to 50 Hz can leave a 50 Hz-only
    // television with no picture at all, and the cadence is handled on the
    // clock instead (see the pacing block above).
    rmode = VIDEO_GetPreferredMode(NULL);

    // Progressive scan, when the hardware and the user both allow it.
    //
    // Three conditions, and all three matter: a component cable has to be
    // plugged in, the console has to be set to progressive (on Wii; on
    // GameCube it is the B button held at boot, which VIDEO_HaveComponentCable
    // does not report), and the mode has to have a progressive counterpart.
    // Asking for 480p without the cable is how a console shows a black screen.
    if (VIDEO_HaveComponentCable()) {
#ifdef TARGET_WII
        const bool wants_progressive = CONF_GetProgressiveScan() > 0;
#else
        const bool wants_progressive = true;
#endif
        if (wants_progressive) {
            switch (rmode->viTVMode >> 2) {
                case VI_NTSC:  rmode = &TVNtsc480Prog;  break;
                case VI_EURGB60: rmode = &TVEurgb60Hz480Prog; break;
                case VI_MPAL:  rmode = &TVMpal480Prog;  break;
                default: break;   // 50 Hz PAL has no progressive mode
            }
        }
    }
    display_is_progressive = (rmode->viTVMode & VI_NON_INTERLACE) != 0;

    // The aspect the *picture* has, which is not the aspect the framebuffer
    // has. PAL 576i renders 640x528, a ratio of 1.212, and the VI shows it as a
    // full 4:3 frame; left to compute its own ratio, gfx_pc lays the HUD out
    // for a screen narrower than the one it is on. Widescreen is the same
    // problem the other way: the frame is stretched and nothing in its
    // dimensions says so.
#ifdef TARGET_WII
    const bool widescreen = CONF_GetAspectRatio() == CONF_ASPECT_16_9;
#else
    // A GameCube has no aspect setting to ask, so the player states it.
    const bool widescreen = configWidescreen;
#endif
    display_aspect = widescreen ? (16.0f / 9.0f) : (4.0f / 3.0f);

    // Take a private copy and force anti-aliasing off.
    //
    // With aa the EFB has to be configured as GX_PF_RGB565_Z16, i.e. a 16-bit
    // depth buffer. SM64 layers coplanar decals -- the sclera, iris and pupil of
    // Mario's eyes, Mario's shadow, painting surfaces -- separated by a very
    // small delta in z. At 16 bits those deltas quantise to the same value, the
    // ordering collapses to "last drawn wins", and the pupil ends up behind the
    // white of the eye.
    //
    // 24-bit depth is worth more to this port than edge anti-aliasing.
    static GXRModeObj mode_copy;
    mode_copy = *rmode;
    mode_copy.aa = 0;
    rmode = &mode_copy;

    // MEM_K0_TO_K1 is mandatory: the XFB must be accessed uncached, otherwise
    // the display shows stale or corrupted data.
    xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    cur_xfb = 0;

    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb[cur_xfb]);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) {
        VIDEO_WaitVSync();
    }
}

// ---- GX bring-up: video-mode dependent, shared with ENABLE_GFX_DUMMY -------
static void gfx_ogc_init_gx(void) {
#ifdef GFX_OGC_BRINGUP_DEBUG
    // A black clear makes "the renderer draws nothing" indistinguishable from
    // "the EFB->XFB copy is broken". Build with -DGFX_OGC_BRINGUP_DEBUG to tell
    // the two apart: if the screen turns blue, everything downstream of drawing
    // works and the fault is in the draw path.
    GXColor background = { 0, 0, 0x80, 0xFF };
#else
    GXColor background = { 0, 0, 0, 0xFF };
#endif

    gp_fifo = memalign(32, OGC_FIFO_SIZE);
    memset(gp_fifo, 0, OGC_FIFO_SIZE);
    GX_Init(gp_fifo, OGC_FIFO_SIZE);

    GX_SetCopyClear(background, GX_MAX_Z24);

    GX_SetViewport(0.0f, 0.0f, rmode->fbWidth, rmode->efbHeight, 0.0f, 1.0f);
    GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);

    u32 xfb_height = GX_SetDispCopyYScale(
        GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight));
    GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopyDst(rmode->fbWidth, xfb_height);
    GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
    GX_SetFieldMode(rmode->field_rendering,
                    (rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE);
    GX_SetPixelFmt(rmode->aa ? GX_PF_RGB565_Z16 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);

    GX_SetCullMode(GX_CULL_NONE);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_CopyDisp(xfb[cur_xfb], GX_TRUE);
}

static void gfx_ogc_copy_to_xfb(void) {
    // This first GX_DrawDone is where the CPU blocks until the GP has finished
    // the frame, so it is the measurement that says whether the GP is the
    // bottleneck. The second one waits only for the copy, which is short.
    const uint64_t gp_wait_start = gfx_perf_enabled() ? gfx_perf_now() : 0;
    GX_DrawDone();
    if (gfx_perf_enabled()) {
        gfx_perf_account(GFX_PERF_GP_WAIT, gp_wait_start);
    }

    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);
    GX_CopyDisp(xfb[cur_xfb], GX_TRUE);
    GX_DrawDone();
}
// ---- end of GX bring-up ----------------------------------------------------

static void gfx_ogc_init(const char *game_name, bool start_in_fullscreen) {
    (void) game_name;
    (void) start_in_fullscreen;

    gfx_ogc_init_video();
    gfx_ogc_init_gx();

    SYS_SetResetCallback(on_reset_pressed);
#ifdef TARGET_WII
    SYS_SetPowerCallback(on_power_pressed);
#endif

    start_ticks = gettime();
    fifty_hz = video_is_50hz();
    next_frame_ticks = start_ticks + microsecs_to_ticks(GAME_FRAME_USEC);
}

// -- GfxWindowManagerAPI -----------------------------------------------------

static void gfx_ogc_set_keyboard_callbacks(bool (*on_key_down)(int scancode),
                                           bool (*on_key_up)(int scancode),
                                           void (*on_all_keys_up)(void)) {
    (void) on_key_down;
    (void) on_key_up;
    (void) on_all_keys_up;
}

static void gfx_ogc_set_fullscreen_changed_callback(void (*on_fullscreen_changed)(bool)) {
    (void) on_fullscreen_changed;
}

static void gfx_ogc_set_fullscreen(bool enable) {
    (void) enable;
}

static void gfx_ogc_main_loop(void (*run_one_game_iter)(void)) {
    // The loop is paced by VIDEO_WaitVSync() in swap_buffers_end, so there is
    // no timing logic here.
    while (!should_quit) {
        gfx_perf_frame_begin();
        run_one_game_iter();
        gfx_perf_frame_end();
    }
    gfx_ogc_shutdown();
}

static void gfx_ogc_get_dimensions(uint32_t *width, uint32_t *height) {
    *width = rmode->fbWidth;
    *height = rmode->efbHeight;
}

static void gfx_ogc_handle_events(void) {
    // Controllers are polled by the ControllerAPI backend.
}

static bool gfx_ogc_start_frame(void) {
    // gfx_start_frame has just recomputed the aspect ratio from the framebuffer
    // dimensions, which is right for a window and wrong for a television.
    // Correcting it here rather than in get_dimensions is deliberate: gfx_pc
    // also uses those dimensions as a pixel count, for the viewport and the
    // scissor, so they have to stay the real ones.
    gfx_pc_override_aspect_ratio(display_aspect);
    return true;
}

static void gfx_ogc_swap_buffers_begin(void) {
    // Deliberately empty. The EFB -> XFB copy used to live here, which was
    // wrong: gfx_pc calls this once per gfx_run(), i.e. once per display list,
    // while GX_CopyDisp(..., GX_TRUE) also *clears* the EFB. When a frame is
    // built from more than one display list -- which is what SM64's intro does,
    // drawing the background and the Goddard Mario head separately -- each copy
    // wiped what the previous one had accumulated, so the head appeared only on
    // the frames where it happened to be last.
    //
    // The copy belongs in swap_buffers_end, which runs exactly once per
    // presented frame. All this hook does now is record that a display list
    // reached the renderer at all -- see swap_buffers_end.
    frame_has_content = true;
}

// SM64 is a 30 fps game: the N64 produces one frame every two VI retraces, and
// the PC backends cap themselves at 1000/30 ms (sync_framerate_with_timer in
// gfx_sdl2.c). Presenting on every retrace runs the whole game -- logic, physics
// and audio -- at double speed. Measured on Dolphin before this was added:
// 59.94 fps instead of 29.97.
//
// Two retraces is exactly right on a 60 Hz mode, and exactly what the N64 did.
// On a 50 Hz PAL mode it gives 25 fps, i.e. the 17% slowdown the original PAL
// release suffered -- and it is not only a speed problem: the audio backend
// feeds the DMA one block per game frame, so at 25 fps it starves continuously.
// See the pacing block near the top of the file for what 50 Hz does instead.

static void gfx_ogc_swap_buffers_end(void) {
    // Only present a frame that was actually drawn.
    //
    // gfx_pc calls this once per game iteration, but swap_buffers_begin (and
    // therefore gfx_run) only when the game submitted a display list. An
    // iteration can pass without one. Copying anyway presents an EFB that the
    // previous GX_CopyDisp(..., GX_TRUE) has already cleared, i.e. a blank
    // frame -- which is the picture appearing and disappearing at a regular
    // beat. OpenGL never showed this: nothing clears its back buffer except
    // gfx_pc itself, so a swap with no drawing re-presents the same image.
    //
    // The pacing below still runs on those iterations: the game's clock must
    // not depend on whether it had anything to draw.
    if (frame_has_content) {
        gfx_ogc_copy_to_xfb();
        VIDEO_SetNextFramebuffer(xfb[cur_xfb]);
        VIDEO_Flush();
    }

    // Everything below is the frame's idle time. A large figure here is the
    // good outcome: it is headroom.
    const uint64_t vsync_start = gfx_perf_enabled() ? gfx_perf_now() : 0;

    if (!fifty_hz) {
        for (int i = 0; i < VSYNCS_PER_FRAME; i++) {
            VIDEO_WaitVSync();
        }
    } else {
        // Always burn at least one retrace, both to bound the loop and to keep
        // the flip synchronised with the scan-out.
        VIDEO_WaitVSync();
        while (gettime() < next_frame_ticks) {
            VIDEO_WaitVSync();
        }
        next_frame_ticks += microsecs_to_ticks(GAME_FRAME_USEC);

        // After a long stall -- a level load, say -- the deadline is far in the
        // past and catching up would run the game fast. Resynchronise instead.
        const u64 now = gettime();
        if (now > next_frame_ticks) {
            next_frame_ticks = now + microsecs_to_ticks(GAME_FRAME_USEC);
        }
    }

    if (gfx_perf_enabled()) {
        gfx_perf_account(GFX_PERF_VSYNC_WAIT, vsync_start);
    }

    if (frame_has_content) {
        cur_xfb ^= 1;
        frame_has_content = false;
    }
}

static double gfx_ogc_get_time(void) {
    // Subtract first: ticks_to_microsecs on a raw uptime loses precision once
    // it is converted to double.
    return (double) ticks_to_microsecs(gettime() - start_ticks) / 1000000.0;
}

uint64_t gfx_ogc_get_ticks(void) {
    return gettime();
}

uint32_t gfx_ogc_get_millis(void) {
    return (uint32_t) ticks_to_millisecs(gettime());
}

uint32_t gfx_ogc_framebuffer_width(void) {
    return rmode->fbWidth;
}

uint32_t gfx_ogc_framebuffer_height(void) {
    return rmode->efbHeight;
}

struct GfxWindowManagerAPI gfx_ogc_wm_api = {
    gfx_ogc_init,
    gfx_ogc_set_keyboard_callbacks,
    gfx_ogc_set_fullscreen_changed_callback,
    gfx_ogc_set_fullscreen,
    gfx_ogc_main_loop,
    gfx_ogc_get_dimensions,
    gfx_ogc_handle_events,
    gfx_ogc_start_frame,
    gfx_ogc_swap_buffers_begin,
    gfx_ogc_swap_buffers_end,
    gfx_ogc_get_time
};

#endif // TARGET_OGC
