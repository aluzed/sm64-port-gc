#ifdef TARGET_OGC

// GameCube controller backend (libogc PAD).
//
// Only <ogc/pad.h> is pulled in, never <gccore.h>: gccore.h drags in ogc/gx.h,
// which declares Vtx and Mtx and collides head-on with the same names in
// PR/gbi.h that ultra64.h brings along. Including the narrow header keeps both
// worlds in the same translation unit.
//
// Wii Remote / Nunchuk / Classic Controller support is STORY-014.

#include <ogc/pad.h>

#include <ultra64.h>

#include "controller_api.h"
#include "controller_ogc.h"

// The GameCube stick reads roughly +/-72 once libogc's calibration is applied,
// while the game expects the N64 range of +/-80 (controller_sdl.c divides the
// +/-32767 SDL axis by 409 to reach the same figure).
#define OGC_STICK_RANGE 72
#define N64_STICK_RANGE 80

// Radial deadzone, in GameCube stick units. Kept deliberately small: SM64 does
// its own filtering, and an aggressive deadzone here makes Mario imprecise on
// narrow platforms.
#define OGC_DEADZONE 8

// Analog triggers count as pressed past ~30% of their travel, matching what
// controller_sdl.c does with SDL's trigger axes.
#define OGC_TRIGGER_THRESHOLD 76

// C-stick deflection needed to latch a C button.
#define OGC_CSTICK_THRESHOLD 40

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void ogc_init(void) {
    PAD_Init();
}

static void ogc_read(OSContPad *pad) {
    // Must happen exactly once per frame. osContGetReadData calls each backend
    // once, so this is the single scan point for the GameCube ports.
    PAD_ScanPads();

    const u16 held = PAD_ButtonsHeld(0);

    if (held & PAD_BUTTON_A)     pad->button |= A_BUTTON;
    if (held & PAD_BUTTON_B)     pad->button |= B_BUTTON;
    if (held & PAD_TRIGGER_Z)    pad->button |= Z_TRIG;
    if (held & PAD_BUTTON_START) pad->button |= START_BUTTON;

    if (held & PAD_BUTTON_UP)    pad->button |= U_JPAD;
    if (held & PAD_BUTTON_DOWN)  pad->button |= D_JPAD;
    if (held & PAD_BUTTON_LEFT)  pad->button |= L_JPAD;
    if (held & PAD_BUTTON_RIGHT) pad->button |= R_JPAD;

    // L doubles as Z so that both reflexes work; they trigger the same action,
    // so there is no conflict.
    if (PAD_TriggerL(0) > OGC_TRIGGER_THRESHOLD) pad->button |= Z_TRIG;
    if (PAD_TriggerR(0) > OGC_TRIGGER_THRESHOLD) pad->button |= R_TRIG;

    // C-stick drives the camera. Y is positive upwards on both the GameCube
    // and the N64, so no inversion here.
    const s8 cx = PAD_SubStickX(0);
    const s8 cy = PAD_SubStickY(0);
    if (cx < -OGC_CSTICK_THRESHOLD) pad->button |= L_CBUTTONS;
    if (cx >  OGC_CSTICK_THRESHOLD) pad->button |= R_CBUTTONS;
    if (cy >  OGC_CSTICK_THRESHOLD) pad->button |= U_CBUTTONS;
    if (cy < -OGC_CSTICK_THRESHOLD) pad->button |= D_CBUTTONS;

    const int sx = PAD_StickX(0);
    const int sy = PAD_StickY(0);

    if (sx * sx + sy * sy > OGC_DEADZONE * OGC_DEADZONE) {
        // Clamp explicitly: a fresh controller can read past OGC_STICK_RANGE,
        // and the game does not expect values beyond the N64 range.
        pad->stick_x = clampi(sx * N64_STICK_RANGE / OGC_STICK_RANGE,
                              -N64_STICK_RANGE, N64_STICK_RANGE);
        pad->stick_y = clampi(sy * N64_STICK_RANGE / OGC_STICK_RANGE,
                              -N64_STICK_RANGE, N64_STICK_RANGE);
    }
}

struct ControllerAPI controller_ogc = {
    ogc_init,
    ogc_read
};

#endif // TARGET_OGC
