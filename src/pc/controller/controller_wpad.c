#ifdef TARGET_WII

// Wii peripherals: Classic Controller (Pro) and Wiimote + Nunchuk, on channel 0.
//
// This file owns the libogc side of the input layer and never includes
// ultra64.h -- see the header for why the two cannot share a translation unit.

#include <math.h>
#include <string.h>

#include <wiiuse/wpad.h>

#include "controller_wpad.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Deadzone on the stick magnitude, which is the quantity the Wii actually
// reports. The GameCube path in controller_ogc.c applies a radial deadzone in
// stick units; this is the same idea in the units available here, `mag` being
// the radius already normalised to [0, 1].
//
// Kept small for the same reason as there: SM64 does its own filtering, and an
// aggressive deadzone costs precision on narrow platforms.
#define WPAD_DEADZONE 0.10f

// Deflection needed on the Classic's right stick to latch a C button, as a
// fraction of full travel. Higher than the movement deadzone: the camera must
// not twitch under a resting thumb.
#define WPAD_CSTICK_THRESHOLD 0.50f

// Travel needed on a Classic analog shoulder to count as pressed. Matches the
// ~30% the GameCube path uses for its analog triggers.
#define WPAD_SHOULDER_THRESHOLD 0.30f

static bool wpad_ready;
static bool wpad_wiimote_alone;

void ogc_wpad_init(void) {
    // Starts the Bluetooth stack, and it is slow -- up to a couple of seconds.
    // It belongs here, at boot, and must never be reached from the frame loop.
    if (WPAD_Init() != WPAD_ERR_NONE) {
        return;
    }

    // The format has to carry extension reports, or a Nunchuk stick never
    // arrives. IR is unused by this backend; the resolution below only scales
    // pointer coordinates nothing here reads, and is set because leaving it
    // unconfigured is what makes the IR-capable formats misbehave.
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
    WPAD_SetVRes(WPAD_CHAN_0, 640, 480);

    wpad_ready = true;
}

// Wii sticks are polar: `mag` in [0, 1] and `ang` in degrees, zero pointing up
// and increasing clockwise. That is the one real difference from the GameCube
// path, whose cartesian deadzone code must not be reused here.
//
// Zero up and clockwise puts x on sin and y on cos, and y comes out positive
// upwards -- the game's convention on the N64 too, so nothing is inverted.
// Results are cartesian in [-1, 1], zeroed inside the deadzone.
static void wpad_stick(const struct joystick_t *js, float deadzone,
                       float *out_x, float *out_y) {
    float mag = js->mag;
    if (mag > 1.0f) {
        mag = 1.0f;
    }
    if (mag < deadzone) {
        *out_x = 0.0f;
        *out_y = 0.0f;
        return;
    }

    const float ang = js->ang * (float) M_PI / 180.0f;
    *out_x = sinf(ang) * mag;
    *out_y = cosf(ang) * mag;
}

static void wpad_movement(const struct joystick_t *js, struct WpadInput *out) {
    float x, y;
    wpad_stick(js, WPAD_DEADZONE, &x, &y);
    out->stick_x = (int) (x * (float) WPAD_IN_STICK_RANGE);
    out->stick_y = (int) (y * (float) WPAD_IN_STICK_RANGE);
}

// A stick standing in for the four C buttons. Thresholding the cartesian result
// rather than the raw angle keeps diagonals behaving like a stick instead of
// like a D-pad.
static unsigned wpad_camera_stick(const struct joystick_t *js) {
    float x, y;
    wpad_stick(js, WPAD_CSTICK_THRESHOLD, &x, &y);

    unsigned b = 0;
    if (x < -WPAD_CSTICK_THRESHOLD) b |= WPAD_IN_C_LEFT;
    if (x >  WPAD_CSTICK_THRESHOLD) b |= WPAD_IN_C_RIGHT;
    if (y >  WPAD_CSTICK_THRESHOLD) b |= WPAD_IN_C_UP;
    if (y < -WPAD_CSTICK_THRESHOLD) b |= WPAD_IN_C_DOWN;
    return b;
}

static unsigned wpad_classic(const classic_ctrl_t *cc, u32 held, struct WpadInput *out) {
    unsigned b = 0;

    if (held & WPAD_CLASSIC_BUTTON_A)    b |= WPAD_IN_A;
    if (held & WPAD_CLASSIC_BUTTON_B)    b |= WPAD_IN_B;
    if (held & WPAD_CLASSIC_BUTTON_PLUS) b |= WPAD_IN_START;

    // Every left-hand shoulder maps to Z so either reflex works, exactly as L
    // doubles as Z on the GameCube pad: they trigger the same action, so there
    // is no conflict.
    if (held & (WPAD_CLASSIC_BUTTON_ZL | WPAD_CLASSIC_BUTTON_ZR)) b |= WPAD_IN_Z;
    if (held & WPAD_CLASSIC_BUTTON_FULL_L)                        b |= WPAD_IN_Z;
    if (held & WPAD_CLASSIC_BUTTON_FULL_R)                        b |= WPAD_IN_R;

    // The analog shoulders latch before their click does. The click sits a long
    // way down and R is used constantly, so a partial pull already counts.
    if (cc->l_shoulder > WPAD_SHOULDER_THRESHOLD) b |= WPAD_IN_Z;
    if (cc->r_shoulder > WPAD_SHOULDER_THRESHOLD) b |= WPAD_IN_R;

    if (held & WPAD_CLASSIC_BUTTON_UP)    b |= WPAD_IN_D_UP;
    if (held & WPAD_CLASSIC_BUTTON_DOWN)  b |= WPAD_IN_D_DOWN;
    if (held & WPAD_CLASSIC_BUTTON_LEFT)  b |= WPAD_IN_D_LEFT;
    if (held & WPAD_CLASSIC_BUTTON_RIGHT) b |= WPAD_IN_D_RIGHT;

    wpad_movement(&cc->ljs, out);
    b |= wpad_camera_stick(&cc->rjs);

    return b;
}

static unsigned wpad_nunchuk(const nunchuk_t *nc, u32 held, struct WpadInput *out) {
    unsigned b = 0;

    if (held & WPAD_BUTTON_A)         b |= WPAD_IN_A;
    if (held & WPAD_BUTTON_B)         b |= WPAD_IN_B;
    if (held & WPAD_BUTTON_PLUS)      b |= WPAD_IN_START;
    if (held & WPAD_NUNCHUK_BUTTON_Z) b |= WPAD_IN_Z;
    if (held & WPAD_NUNCHUK_BUTTON_C) b |= WPAD_IN_R;

    // The Wiimote is held upright in this configuration, so its D-pad reads
    // naturally. It drives the camera rather than the N64 D-pad: there is no
    // second stick to spare, the camera is used constantly, and the N64 D-pad
    // only serves debug menus.
    if (held & WPAD_BUTTON_UP)    b |= WPAD_IN_C_UP;
    if (held & WPAD_BUTTON_DOWN)  b |= WPAD_IN_C_DOWN;
    if (held & WPAD_BUTTON_LEFT)  b |= WPAD_IN_C_LEFT;
    if (held & WPAD_BUTTON_RIGHT) b |= WPAD_IN_C_RIGHT;

    wpad_movement(&nc->js, out);

    return b;
}

bool ogc_wpad_read(struct WpadInput *out) {
    memset(out, 0, sizeof(*out));
    wpad_wiimote_alone = false;

    if (!wpad_ready) {
        return false;
    }

    WPAD_ScanPads();

    // Probe rather than trusting the last good read. It reports an error as
    // soon as the peripheral is gone, which is the hook for hot-unplug: fall
    // through with *out zeroed, so the game sees a released stick instead of
    // the last one held. Without that, pulling a Nunchuk leaves Mario running.
    u32 type = 0;
    if (WPAD_Probe(WPAD_CHAN_0, &type) != WPAD_ERR_NONE) {
        return false;
    }

    const WPADData *wd = WPAD_Data(WPAD_CHAN_0);
    if (wd == NULL) {
        return false;
    }

    const u32 held = wd->btns_h;

    switch (wd->exp.type) {
        case WPAD_EXP_CLASSIC:
            out->buttons = wpad_classic(&wd->exp.classic, held, out);
            return true;
        case WPAD_EXP_NUNCHUK:
            out->buttons = wpad_nunchuk(&wd->exp.nunchuk, held, out);
            return true;
        default:
            // A Wiimote on its own, or an expansion this backend does not
            // speak. Refused deliberately: SM64 without an analog stick is not
            // a harder game, it is a different and worse one, and a digital
            // mapping would ship that as though it were support.
            wpad_wiimote_alone = (wd->exp.type == WPAD_EXP_NONE);
            return false;
    }
}

bool ogc_wpad_needs_nunchuk(void) {
    return wpad_wiimote_alone;
}

#endif // TARGET_WII
