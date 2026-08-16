# STORY-013 — GameCube controller (`PAD`) → `OSContPad`

**Epic:** 4 — Input
**Status:** 🟡 Implemented, unvalidated on hardware (`src/pc/controller/controller_ogc.c`)
**Depends on:** STORY-003 *(parallelisable with the GX epic)*
**Estimate:** S (1-2 d)
**Platform:** GC + Wii (GameCube controller ports)

## Context

`ControllerAPI` is minimal:

```c
struct ControllerAPI {
    void (*init)(void);
    void (*read)(OSContPad *pad);
};
```

`OSContPad` is the original N64 structure: a 16-bit `button` field and two `s8` axes,
`stick_x` / `stick_y`. The button constants live in `include/PR/os_cont.h` (`A_BUTTON`,
`B_BUTTON`, `Z_TRIG`, `R_TRIG`, `L_TRIG`, `START_BUTTON`, `U/D/L/R_JPAD`,
`U/D/L/R_CBUTTONS`), and `VALID_BUTTONS` in `include/sm64.h` lists the ones the game uses.

Useful reference: `controller_sdl.c` divides the SDL axis (±32767) by 409, which gives an
**N64 range of ±80**. That is the target for converting the GameCube stick.

The GameCube controller has more buttons than the N64, so the mapping is comfortable with no
compromises.

## Goal

As a player, I want to control Mario with a GameCube controller through a natural mapping, so
I can play on a console.

## Acceptance criteria

- [ ] Mario moves in every direction with full range: slow walk near centre, running at full
      deflection.
- [ ] No drift at rest (dead zone applied correctly).
- [ ] The game's eight actions respond: jump (A), attack/grab (B), crouch/dive (Z), camera
      (C-stick, R), pause (Start).
- [ ] The stick is **circular**: pushing diagonally at full deflection is not faster than
      pushing straight.
- [ ] Hot-plugging a controller is detected without restarting the game.
- [ ] The mapping is documented in `README.md`.

## Mapping

| GameCube | N64 | Role in SM64 |
|---|---|---|
| Main stick | analog stick | movement |
| A | `A_BUTTON` | jump |
| B | `B_BUTTON` | attack / grab / talk |
| Z button **or** analog L (> 30 %) | `Z_TRIG` | crouch, dive, ground pound |
| Analog R (> 30 %) | `R_TRIG` | camera mode |
| Start | `START_BUTTON` | pause |
| C-stick (4 directions, ~50 % threshold) | `U/D/L/R_CBUTTONS` | camera |
| D-pad | `U/D/L/R_JPAD` | menus, debug mode |
| X, Y | — | free (optional shortcuts) |

Mapping `Z_TRIG` onto **both** the Z button and the L trigger covers both player reflexes with
no conflict, since both trigger the same action.

## Tasks

1. **Create `src/pc/controller/controller_ogc.c`** exporting `struct ControllerAPI controller_ogc`.

2. **`init()`**: `PAD_Init();` — libogc handles detection and hot-plug on its own.

3. **`read(OSContPad *pad)`**: `PAD_ScanPads()`, then map `PAD_ButtonsHeld(0)` onto the N64
   bits, plus the analog triggers past ~30 % of travel. Do not zero `pad->stick_x/y` up front:
   zero them *inside* the dead-zone branch, the way `controller_sdl.c` does.

4. **C-stick → C buttons.** `PAD_SubStickX/Y(0)` returns roughly ±80; threshold at ~40. Mind
   the **Y sign**: `PAD_StickY` is positive upwards, like the N64. Do not flip it by reflex.

5. **Main stick, radial dead zone.** Mirror `controller_sdl.c`: compute the squared magnitude,
   compare against `DEADZONE²`, and only convert beyond it. The GameCube stick reads roughly
   ±72 to ±80 depending on the pad and its wear: apply a scale towards ±80 with **explicit
   clamping**, or a fresh controller will saturate past the range the game expects.

6. **Registration** in `src/pc/controller/controller_entry_point.c` under `#ifdef TARGET_OGC`.

7. **Exposed settings** (groundwork for STORY-015): dead zone, trigger threshold, C-stick
   threshold, optional camera Y inversion — in `configfile.c`.

## Files touched

- `src/pc/controller/controller_ogc.c`, `controller_ogc.h` (new)
- `src/pc/controller/controller_entry_point.c`
- `src/pc/configfile.c` / `configfile.h`
- `Makefile`

## Notes and risks

- `PAD_ScanPads()` must be called **once per frame**. `read()` is called once per frame by the
  game, so that is natural — but if a second backend (STORY-014) is ever active at the same
  time, the scan will need to move to a single entry point.
- GameCube stick calibration drifts with wear. libogc recalibrates on plug (`PAD_Init` /
  `PAD_Reset`); do not add a homemade recalibration, it makes things worse.
- SM64 already applies its own dead-zone handling in `src/game/game_init.c`. Too wide a dead
  zone at the backend level makes Mario imprecise in tight platforming — start small and tune
  with a controller in hand.
- On Wii this backend covers the GameCube controller ports; Wii Remotes are STORY-014. The two
  must be able to coexist.

## Implementation note

Only `<ogc/pad.h>` is included, never `<gccore.h>`: the latter drags in `ogc/gx.h`, whose `Vtx`
and `Mtx` collide head-on with the same names from `PR/gbi.h` that `ultra64.h` brings along
(see STORY-003). Including the narrow header keeps both worlds in one translation unit.
