# STORY-001 — Repo hygiene: keep ROM archives out of version control

**Epic:** 0 — Foundations
**Status:** ✅ Done
**Depends on:** —
**Estimate:** XS
**Platform:** all

## Context

The repository root holds the ROM archives used by `extract_assets.py`:

```
Super Mario 64 (Europe) (En,Fr,De).zip
Super Mario 64 (USA).zip
```

The stock `.gitignore` already covered `*.z64` but not the archives containing them.
Committing those would be both a legal problem (Nintendo copyrighted material) and a
practical one: 12 MB of binaries in history, impossible to remove afterwards without
rewriting the whole thing.

## Goal

As the fork maintainer, I want ROM archives and raw ROM images ignored by Git, so that no
contribution can commit them by accident.

## Acceptance criteria

- [x] `git status` no longer lists the two root `.zip` files.
- [x] `git check-ignore -v "Super Mario 64 (USA).zip"` reports the `.gitignore` rule.
- [x] ROM image extensions not already covered (`.n64`, `.v64`) are ignored too, alongside
      the existing `.z64`.
- [x] No archive was tracked yet, so no `git rm --cached` was needed.

## What was done

Block added to `.gitignore`, just before the general project-specific ignores:

```gitignore
# ROM archives / base ROMs (never commit copyrighted material)
*.zip
*.7z
*.rar
*.n64
*.v64
```

## Notes

- `*.zip` is deliberately broad. If a legitimate `.zip` ever needs versioning (a custom
  asset, a release archive), it will need an explicit exception such as `!/docs/**/*.zip`,
  mirroring the `*custom*` rules already used for assets.
- The project `README.md` must keep stating, in the GC/Wii build section, that the base ROM
  is the user's to provide — the repository does not distribute it.

## Files touched

- `.gitignore`
