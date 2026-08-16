# STORY-014 — Wii controllers (`WPAD`): Wiimote+Nunchuk, Classic, GC

**Epic:** 4 — Input
**Status:** To do
**Depends on:** STORY-013
**Estimate:** M (2-3 d)
**Platform:** Wii only

## Context

On Wii the GameCube controller (STORY-013) works as is if the player plugs one in, but most
players do not have one. So we need Wii peripherals through `WPAD` (libogc, on top of
`libwiiuse` and `libbte`).

Three configurations to cover, by play quality:

| Peripheral | Analog stick | Verdict for SM64 |
|---|---|---|
| Classic Controller (Pro) | yes, two sticks | **ideal** — equivalent to GameCube |
| Wiimote + Nunchuk | yes (Nunchuk) | **very good** — one stick, camera on the D-pad |
| Wiimote alone | no | **unsuitable** — SM64 needs an analog stick |

A Wiimote on its own cannot play the game properly: slow walking, jump precision and camera
control all depend on the analog stick. The position taken is to **detect it and show a
message** asking for a Nunchuk, rather than shipping a frustrating digital mapping.

## Goal

As a Wii player, I want to play with a Classic Controller or a Wiimote + Nunchuk, so I do not
need a GameCube controller.

## Acceptance criteria

- [ ] The game detects the peripheral on channel 0 automatically and adapts the mapping without
      a restart.
- [ ] Classic Controller: full mapping, as comfortable as the GameCube pad.
- [ ] Wiimote + Nunchuk: Mario moves with the Nunchuk stick, jumps and attacks, camera is
      controllable.
- [ ] Unplugging the Nunchuk mid-game neither crashes the game nor sends Mario running forever
      (inputs must be zeroed).
- [ ] Wiimote alone: explicit message, no crash.
- [ ] A GameCube controller and a Wii peripheral can be plugged in at once with no input
      conflict (one of them takes over, deterministically).
- [ ] The HOME button returns to the Homebrew Channel (see STORY-019).

## Mappings

### Classic Controller

| Classic | N64 |
|---|---|
| Left stick | analog stick |
| a | `A_BUTTON` |
| b | `B_BUTTON` |
| ZL / ZR | `Z_TRIG` |
| Right trigger (analog) | `R_TRIG` |
| `+` | `START_BUTTON` |
| Right stick (4 directions) | C buttons |
| D-pad | N64 D-pad |

### Wiimote + Nunchuk

| Wiimote / Nunchuk | N64 |
|---|---|
| Nunchuk stick | analog stick |
| A (Wiimote) | `A_BUTTON` |
| B (Wiimote trigger) | `B_BUTTON` |
| Z (Nunchuk) | `Z_TRIG` |
| C (Nunchuk) | `R_TRIG` (recentre camera) |
| `+` | `START_BUTTON` |
| Wiimote D-pad | C buttons (camera) |
| Shake (accelerometer) | — *out of scope for v1* |

The Wiimote D-pad drives the camera rather than the N64 D-pad, because the camera is used
constantly while the N64 D-pad only serves debug menus.

## Tasks

1. **Initialisation** in `controller_ogc.c` under `#ifdef TARGET_WII`: `WPAD_Init()`,
   `WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR)`, `WPAD_SetVRes(...)`.

2. **Extension detection** on every read, switching on `wd->exp.type`
   (`WPAD_EXP_CLASSIC`, `WPAD_EXP_NUNCHUK`, default).

3. **Stick conversion.** Wii sticks are exposed in polar coordinates (`js->mag` ∈ [0,1] and
   `js->ang` in degrees, 0 = up, clockwise), not cartesian:
   ```c
   float mag = js->mag;  if (mag > 1.0f) mag = 1.0f;
   float ang = js->ang * (float)M_PI / 180.0f;
   int x =  (int)(sinf(ang) * mag * 80.0f);
   int y =  (int)(cosf(ang) * mag * 80.0f);
   ```
   That is the main difference from STORY-013: **do not reuse** the cartesian dead-zone code
   as is. Here the dead zone applies directly to `mag`.

4. **Backend priority.** Pick a simple, deterministic rule: the GameCube controller in port 1
   wins if plugged in, otherwise the Wiimote on channel 0. Re-evaluate once a second, not every
   frame — hot-plug does not need to be instantaneous, and polling both subsystems constantly
   costs.

5. **Hot-unplug.** `WPAD_Probe(WPAD_CHAN_0, &type)` returns an error when the peripheral is
   gone. In that case `memset(pad, 0, sizeof(*pad))` and return — otherwise the last stick
   value stays applied and Mario runs forever.

6. **Rumble** (optional, `sh` version only): `WPAD_Rumble(chan, on)` and
   `PAD_ControlMotor(chan, PAD_MOTOR_RUMBLE)`. SM64 only drives rumble in the Shindou version
   (`VERSION=sh`); wire it to the same entry point as the Rumble Pak. **Low priority.**

7. **Linking**: add `-lwiiuse -lbte` to `PLATFORM_LDFLAGS` for the Wii target (STORY-002).
   These libraries do not exist on the GameCube side, so guard them properly.

## Files touched

- `src/pc/controller/controller_ogc.c`
- `src/pc/controller/controller_entry_point.c`
- `Makefile` (`-lwiiuse -lbte` on Wii only)

## Notes and risks

- `WPAD_Init()` starts the Bluetooth stack: it is **slow** (up to ~2 s) and must happen once at
  boot, never in the game loop.
- Trigonometric functions in the input loop are fine (once per frame), but `libm` adds to the
  binary: check the impact on the STORY-005 budget.
- Wiimotes power off after a few minutes of inactivity. That is not a bug to fix, but the
  reconnection code must be robust (see task 5).
- Dolphin emulates Wiimotes through the keyboard or a real Bluetooth Wiimote. The Nunchuk and
  Classic mappings test correctly on Dolphin; hot-unplug is better tested on hardware.
