#ifndef COMPAT_H
#define COMPAT_H value

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#define __BSD__
#endif

// SM64 relies on `char` being signed all over the place (s8 counters that run
// negative, angle bytes, sentinel indices). x86 gives us that for free, but
// PowerPC defaults to unsigned char, and the mismatch produces a game that
// builds cleanly and then misbehaves in ways that are painful to trace back.
// The Makefile passes -fsigned-char for the console targets; this catches the
// case where it ever gets dropped.
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert((char) -1 < 0, "sm64 requires a signed char; compile with -fsigned-char");
#endif

#endif