# STORY-022 — Alignment exception entering the castle, and the audit it implies

**Epic:** 0 — Foundations
**Status:** 🟡 Cause found and fixed; **awaiting hardware confirmation**, then an audit
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

1. **Confirm on hardware.** Enter the castle. Nothing else proves it.
2. **Audit for the same class.** This one was found by crashing; there is no reason to think it
   is the only one. Places worth a pass:
   - every allocator with a bump pointer, and every caller passing a byte count rather than a
     `sizeof` — `level_cmd_set_terrain_data` was the only one in `src/`, but `src/pc` should be
     checked too;
   - floats read or written through a pointer derived from a byte buffer, which is the shape
     that traps;
   - anything DMA'd, where 32-byte alignment is required and the failure is silent corruption
     instead of an exception.
3. **Make the failure loud instead of remote.** A debug-only check in `alloc_only_pool_alloc`
   that traps immediately on returning a misaligned pointer would have named the culprit
   directly, rather than pointing at the innocent graph node that used it. Cheap, and it turns
   a class of hardware-only bug into something a single build reveals.
4. **Consider whether Dolphin can be made stricter.** If some configuration raises alignment
   exceptions instead of emulating them, the emulator loop regains coverage of this class.
   Worth twenty minutes to find out; if not, record that it cannot and move on.

## Acceptance criteria

- [ ] The castle loads on a real GameCube.
- [ ] The audit in task 2 has been run and its result recorded here, including "found nothing"
      if that is the answer.
- [ ] The guard in task 3 exists, or a note says why it was not worth it.
