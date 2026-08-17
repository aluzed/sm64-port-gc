#ifndef CONTROLLER_WPAD_H
#define CONTROLLER_WPAD_H

#ifdef TARGET_WII

#include <stdbool.h>

// Wii peripherals sit behind this narrow interface for one concrete reason.
//
// <wiiuse/wpad.h> reaches <wiiuse/wiiuse.h>, which on GEKKO includes
// <bte/bte.h>, which includes <gccore.h>, which brings in <ogc/gx.h> -- and the
// Vtx and Mtx declared there collide head-on with the same names in PR/gbi.h,
// which ultra64.h supplies. controller_ogc.c needs ultra64.h for the N64 button
// bits, so those two headers cannot meet in one translation unit. The same trap
// is already documented at the top of controller_ogc.c for <gccore.h>; WPAD
// walks into it through a longer chain.
//
// Hence: nothing below names a libogc type or an ultra64 type. The libogc side
// lives entirely in controller_wpad.c, the ultra64 side entirely in
// controller_ogc.c, and this header is the seam.

// Neutral button set, translated onto the N64 bits by controller_ogc.c -- the
// only file allowed to know both vocabularies.
enum {
    WPAD_IN_A       = 1u << 0,
    WPAD_IN_B       = 1u << 1,
    WPAD_IN_Z       = 1u << 2,
    WPAD_IN_R       = 1u << 3,
    WPAD_IN_START   = 1u << 4,
    WPAD_IN_C_UP    = 1u << 5,
    WPAD_IN_C_DOWN  = 1u << 6,
    WPAD_IN_C_LEFT  = 1u << 7,
    WPAD_IN_C_RIGHT = 1u << 8,
    WPAD_IN_D_UP    = 1u << 9,
    WPAD_IN_D_DOWN  = 1u << 10,
    WPAD_IN_D_LEFT  = 1u << 11,
    WPAD_IN_D_RIGHT = 1u << 12,
};

// Each stick axis is delivered already scaled to the range the game expects,
// the same one controller_ogc.c targets for the GameCube stick. Converting here
// rather than at the seam keeps the polar-to-cartesian work in the one file
// that knows the sticks are polar to begin with.
#define WPAD_IN_STICK_RANGE 80

struct WpadInput {
    unsigned buttons;
    int stick_x;
    int stick_y;
};

void ogc_wpad_init(void);

// Fills *out and returns true when a usable Wii peripheral is driving the game.
//
// Returns false -- with *out zeroed -- when there is nothing on channel 0, when
// the peripheral has just been unplugged, or when it is a Wiimote with no
// extension. Zeroing matters: leaving the last stick value applied is what
// makes Mario run forever after a Nunchuk is pulled out.
bool ogc_wpad_read(struct WpadInput *out);

// True when channel 0 holds a Wiimote with no extension. SM64 needs an analog
// stick -- slow walking, jump precision and camera control all depend on it --
// so this configuration is refused rather than mapped onto the D-pad. Exposed
// so that a later story can say so on screen; see STORY-014 and STORY-019.
bool ogc_wpad_needs_nunchuk(void);

// True while HOME is held, on the Wiimote or on a Classic Controller.
//
// Read independently of everything above, and deliberately so: it must work
// when ogc_wpad_read has just refused the peripheral. A player holding a
// Wiimote with no Nunchuk cannot play, and would otherwise have no way out
// except pulling the plug -- which is the whole thing STORY-019 exists to
// prevent.
bool ogc_wpad_home_held(void);

#endif // TARGET_WII

#endif
