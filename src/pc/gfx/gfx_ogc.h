#ifndef GFX_OGC_H
#define GFX_OGC_H

#ifdef TARGET_OGC

#include <stdbool.h>
#include <stdint.h>

#include "gfx_window_manager_api.h"

extern struct GfxWindowManagerAPI gfx_ogc_wm_api;

// Resolution actually selected at boot. The renderer needs it to flip the Y
// axis of viewports and scissors (gfx_pc uses OpenGL conventions, GX does not).
uint32_t gfx_ogc_framebuffer_width(void);
uint32_t gfx_ogc_framebuffer_height(void);

// Asks the main loop to shut down cleanly at the end of the current frame.
// Safe to call from an interrupt context (RESET/POWER callbacks do).
void gfx_ogc_request_quit(bool power_off);

// Time Base Register, monotonic since boot.
//
// This wrapper exists because libogc's gettime() is a static inline living in
// <ogc/lwp_watchdog.h>, a header that reaches ogc/gx.h and therefore cannot be
// included next to ultra64.h (their Vtx/Mtx declarations clash -- see
// docs/stories/003). osGetTime() in ultra_reimplementation.c calls this instead.
uint64_t gfx_ogc_get_ticks(void);

#endif // TARGET_OGC

#endif
