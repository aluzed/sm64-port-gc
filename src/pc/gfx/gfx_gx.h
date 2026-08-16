#ifndef GFX_GX_H
#define GFX_GX_H

#ifdef ENABLE_GX

#include "gfx_rendering_api.h"

// Deliberately free of any libogc header. <ogc/gx.h> declares Vtx and Mtx, which
// collide with the ones PR/gbi.h declares through ultra64.h -- and pc_main.c
// includes both this header and sm64.h. See docs/stories/003.
extern struct GfxRenderingAPI gfx_gx_api;

#endif // ENABLE_GX

#endif
