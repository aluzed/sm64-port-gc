# STORY-022 — Alignment exception entering the castle, and the audit it implies

**Epic:** 0 — Foundations
**Status:** ✅ **Done** — fixed and confirmed on hardware, audit run, guard in place
**Depends on:** STORY-003
**Estimate:** S for the fix (done), M for the audit
**Platform:** GC + Wii

## The crash

A real GameCube, opening the castle door. Outside was fine.

```
Exception (Alignment) occurred!
PC 80005B64   LR 80005B40
GPR03 80FE73AA
0x80005b64 --> 0x000000a0 --> 0x80004f30 --> 0x800059fc -->
0x800087cc --> 0x80008edc --> 0x8004a38c --> 0x800ced8c --> ...
```

## How it was found, in two commands

Worth recording, because it took minutes rather than a session:

```sh
powerpc-eabi-addr2line -e build/us_gc/sm64.us.elf -f -C -i 80005B64 80004f30 800059fc ...
#   init_graph_node_ortho_projection
#   geo_layout_cmd_node_ortho_projection
#   process_geo_layout
#   level_cmd_begin_area  <- loading a new area, i.e. the door

powerpc-eabi-objdump -d --start-address=0x80005b20 --stop-address=0x80005b80 \
    build/us_gc/sm64.us.elf
#   80005b5c:  stw   r3,4(r3)
#   80005b60:  stw   r3,8(r3)
#   80005b64:  stfs  f1,20(r3)   <- r3 = 0x80FE73AA, EA = 0x80FE73BE
```

Disassembling mattered. The symbol name alone suggests a projection or a matrix problem; the
instruction says plainly that a float was stored to a 2-mod-4 address, and that the two integer
stores on the lines above did the same thing and got away with it.

## Cause and fix

`alloc_only_pool_alloc`, `USE_SYSTEM_MALLOC` variant, advanced its bump pointer by the raw
requested size. `level_cmd_set_terrain_data` allocates
`get_area_terrain_size() * sizeof(Collision)` bytes with `Collision` an `s16`, so an area with
an odd number of collision entries leaves the pool two bytes out for everything after it. Fixed
by rounding the bump to 8, in the allocator so every caller is covered. See
[STORY-003](003-c-portability-powerpc-traps.md).

## Why nothing caught it earlier

| Layer | Why it stayed hidden |
|---|---|
| x86 PC builds | unaligned accesses just work; latent upstream, probably for years |
| Dolphin | its JIT performs the access rather than raising the exception |
| PowerPC integer ops | unaligned `lwz`/`stw` are fixed up in hardware — only floats trap |

The third is the nastiest: the fault surfaces at whichever float touches the misaligned region
first, arbitrarily far from the allocation that caused it.

## Tasks

1. ✅ **Confirmed on hardware.** The castle loads, the first level is playable, audio holds.
2. ✅ **Audited.** Result below. The short version: no second instance found, and the tool that
   looked is not worth keeping as a gate. Places checked:
   - every allocator with a bump pointer, and every caller passing a byte count rather than a
     `sizeof` — `level_cmd_set_terrain_data` was the only one in `src/`, but `src/pc` should be
     checked too;
   - floats read or written through a pointer derived from a byte buffer, which is the shape
     that traps;
   - anything DMA'd, where 32-byte alignment is required and the failure is silent corruption
     instead of an exception.
3. ✅ **Done.** `-DCHECK_POOL_ALIGNMENT` stops the moment a pool returns a misaligned pointer,
   instead of at whichever float first touches it. Verified not to fire through boot and the
   attract demo, so every pool allocation on that path is 8-aligned.
4. ⛔ **Dropped: whether Dolphin can be made to raise alignment exceptions.** It would only buy
   back emulator coverage of a class that is already covered where it matters, by the guard in
   task 3 — which checks the property at the one place it can be violated, on hardware and in
   the emulator alike. Not worth carrying as an open item.

## Acceptance criteria

- [x] The castle loads on a real GameCube.
- [x] The audit in task 2 has been run and its result recorded here.
- [x] The guard in task 3 exists.
- [x] Task 4 closed as dropped, with the reason recorded above.

## Audit result

**`-Wcast-align=strict` over the whole build: about 320 hits, and no second bug.**

| Where | Hits | Verdict |
|---|---|---|
| `src/engine/level_script.c`, `geo_layout.[ch]` | 212 | command streams that are 4-aligned by construction — the `CMD_GET` macros. Not a hazard |
| `src/goddard/*` | 68 | same shape, dynlist commands |
| `src/audio/*` | 24 | N64 audio data, aligned by construction |
| `src/game/save_file.c` | 4 | `SaveBlockSignature` is two `u16`s, so it needs 2-byte alignment and gets it |
| `src/pc/storage_ogc.c` | 4 | ours, false positives: the buffer is declared `aligned(32)`. Rewritten as a union of the header and the bytes, which removes the cast and states the intent in the type instead of in a comment |

**The tool is too blunt to keep as a gate.** It cannot tell a pointer into a structurally
aligned command stream from one into a heap block, and everything it flagged here is the former.
Turning it on permanently would mean 320 warnings to ignore, which is worse than none.

What actually protects this port is task 3: the guard checks the property that matters, at the
one place where it can be violated, and says so immediately. One bug of this class was found by
crashing a console; the next one will name itself.

The other real defence is already in place elsewhere and worth listing, since it is the same
family: every buffer the GP or the DSP reads by DMA is `memalign(32, ...)` or
`__attribute__((aligned(32)))` and flushed with `DCFlushRange`. Those were written blind against
the hardware manual and the first hardware run confirmed them.
