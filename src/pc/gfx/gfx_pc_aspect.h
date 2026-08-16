#ifndef GFX_PC_ASPECT_H
#define GFX_PC_ASPECT_H

// Override the aspect ratio gfx_pc derives from the framebuffer dimensions.
//
// gfx_start_frame computes aspect_ratio as width / height, which is right on a
// PC window and wrong on a television. Two cases where they differ:
//
//  * PAL 576i renders 640x528, a ratio of 1.212, and the VI displays it as a
//    full 4:3 frame. Left alone, the HUD is laid out for a screen narrower
//    than the one it is on and sits inset by about fifteen units a side.
//  * A 16:9 display shows the same 640x480 frame stretched. Nothing about the
//    framebuffer says so; only the console's setting does.
//
// This header carries no dependencies on purpose. gfx_pc.h declares
// gfx_run(Gfx *), which drags in PR/gbi.h, whose Vtx and Mtx clash with the
// ones <ogc/gx.h> declares -- so a libogc translation unit cannot include it
// (docs/stories/003).
//
// Call once per frame, after gfx_start_frame, or the recomputed value wins.

#ifdef __cplusplus
extern "C" {
#endif

void gfx_pc_override_aspect_ratio(float aspect_ratio);

#ifdef __cplusplus
}
#endif

#endif
