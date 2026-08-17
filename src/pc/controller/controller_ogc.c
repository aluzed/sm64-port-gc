#ifdef TARGET_OGC

// GameCube controller backend (libogc PAD).
//
// Only <ogc/pad.h> is pulled in, never <gccore.h>: gccore.h drags in ogc/gx.h,
// which declares Vtx and Mtx and collides head-on with the same names in
// PR/gbi.h that ultra64.h brings along. Including the narrow header keeps both
// worlds in the same translation unit.
//
// Wii Remote, Nunchuk and Classic Controller support lives in
// controller_wpad.c, behind a header that names no libogc type. WPAD's own
// header walks into the same collision by a longer route -- <wiiuse/wpad.h> ->
// <bte/bte.h> -> <gccore.h> -> <ogc/gx.h> -- so it cannot be included here
// either. This file is the only one that knows both the neutral vocabulary and
// the N64 button bits, which is exactly its job.

#include <ogc/pad.h>

#include <ultra64.h>

#include "../gfx/gfx_ogc.h"

#include "controller_api.h"
#include "controller_ogc.h"
#include "controller_wpad.h"

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

// How long Start + X + Y must be held before the game exits.
//
// The GameCube has no HOME button, so the way out has to be a combination --
// and a combination that quits the game cannot be one a player can hit by
// accident mid-jump. X and Y are mapped to nothing in SM64, so the pair is free
// and the hold is what makes it deliberate.
#define OGC_EXIT_HOLD_MS 1000

// Start + X + Y hold state, for the exit combination above.
static bool exit_combo_held;
static uint32_t exit_combo_since;

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void ogc_init(void) {
    PAD_Init();
#ifdef TARGET_WII
    ogc_wpad_init();
#endif
}

#ifdef TARGET_WII
// Translates the neutral button set from controller_wpad.c onto the N64 bits.
// Splitting the mapping in two -- peripheral to neutral there, neutral to N64
// here -- is what keeps the two header worlds apart; see the note at the top.
static void ogc_apply_wpad(const struct WpadInput *in, OSContPad *pad) {
    if (in->buttons & WPAD_IN_A)       pad->button |= A_BUTTON;
    if (in->buttons & WPAD_IN_B)       pad->button |= B_BUTTON;
    if (in->buttons & WPAD_IN_Z)       pad->button |= Z_TRIG;
    if (in->buttons & WPAD_IN_R)       pad->button |= R_TRIG;
    if (in->buttons & WPAD_IN_START)   pad->button |= START_BUTTON;

    if (in->buttons & WPAD_IN_C_UP)    pad->button |= U_CBUTTONS;
    if (in->buttons & WPAD_IN_C_DOWN)  pad->button |= D_CBUTTONS;
    if (in->buttons & WPAD_IN_C_LEFT)  pad->button |= L_CBUTTONS;
    if (in->buttons & WPAD_IN_C_RIGHT) pad->button |= R_CBUTTONS;

    if (in->buttons & WPAD_IN_D_UP)    pad->button |= U_JPAD;
    if (in->buttons & WPAD_IN_D_DOWN)  pad->button |= D_JPAD;
    if (in->buttons & WPAD_IN_D_LEFT)  pad->button |= L_JPAD;
    if (in->buttons & WPAD_IN_D_RIGHT) pad->button |= R_JPAD;

    pad->stick_x = clampi(in->stick_x, -N64_STICK_RANGE, N64_STICK_RANGE);
    pad->stick_y = clampi(in->stick_y, -N64_STICK_RANGE, N64_STICK_RANGE);
}
#endif

static void ogc_read(OSContPad *pad) {
    // Must happen exactly once per frame. osContGetReadData calls each backend
    // once, so this is the single scan point for the GameCube ports.
    const u32 gc_connected = PAD_ScanPads();

#ifdef TARGET_WII
    // Arbitration, and it is deliberately not the once-a-second re-evaluation
    // STORY-014 sketched. That rule existed to avoid polling both subsystems
    // every frame, but neither call is a transaction: PAD_ScanPads reads the SI
    // registers and WPAD_ScanPads latches data the Bluetooth stack has already
    // delivered on its own thread. The traffic the rule was protecting against
    // happens whether or not we ask for it.
    //
    // So arbitrate every frame instead, which costs nothing measurable and
    // removes the second of latency a player would feel swapping controllers.
    // The rule itself is unchanged and deterministic: a GameCube pad in port 1
    // wins, otherwise channel 0.
    struct WpadInput wii;
    const bool wii_usable = ogc_wpad_read(&wii);

    // HOME quits, whatever the arbitration decided and whatever is plugged in.
    // It is read even when a GameCube pad has won the port, because a player
    // holding a Wiimote should not have to work out which controller the game
    // is currently listening to in order to get out of it.
    if (ogc_wpad_home_held()) {
        gfx_ogc_request_quit(false);
    }

    if (!(gc_connected & 1)) {
        if (wii_usable) {
            ogc_apply_wpad(&wii, pad);
        }
        // Nothing usable on channel 0 -- no peripheral, or a Wiimote with no
        // Nunchuk. Leave the pad as osContGetReadData zeroed it and return:
        // there is no GameCube pad to read either.
        return;
    }
#else
    (void) gc_connected;
#endif

    const u16 held = PAD_ButtonsHeld(0);

    // Start + X + Y, held. Timed on the clock rather than counted in frames,
    // because the frame rate is 30 or 25 depending on the video mode and "one
    // second" should not mean 1.2 s on a PAL console.
    if ((held & (PAD_BUTTON_START | PAD_BUTTON_X | PAD_BUTTON_Y))
        == (PAD_BUTTON_START | PAD_BUTTON_X | PAD_BUTTON_Y)) {
        const uint32_t now = gfx_ogc_get_millis();
        if (!exit_combo_held) {
            exit_combo_held = true;
            exit_combo_since = now;
        } else if (now - exit_combo_since >= OGC_EXIT_HOLD_MS) {
            gfx_ogc_request_quit(false);
        }
    } else {
        exit_combo_held = false;
    }

    if (held & PAD_BUTTON_A)     pad->button |= A_BUTTON;
    if (held & PAD_BUTTON_B)     pad->button |= B_BUTTON;
    if (held & PAD_TRIGGER_Z)    pad->button |= Z_TRIG;

    // Start passes through unless X and Y are down with it: otherwise every
    // attempt to quit opens the pause menu on the way out. X and Y are mapped
    // to nothing, so this costs the player nothing they could have wanted.
    if ((held & PAD_BUTTON_START)
        && !((held & (PAD_BUTTON_X | PAD_BUTTON_Y)) == (PAD_BUTTON_X | PAD_BUTTON_Y))) {
        pad->button |= START_BUTTON;
    }

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
