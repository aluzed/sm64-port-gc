#ifndef STORAGE_OGC_H
#define STORAGE_OGC_H

#include <stdbool.h>

// Persistent storage for the GameCube and Wii builds.
//
// This header is deliberately free of libogc includes so it can be used from
// translation units that also include ultra64.h -- <ogc/gx.h> and PR/gbi.h
// declare Vtx and Mtx incompatibly, and ultra_reimplementation.c needs both
// this and the GBI (docs/stories/003).

#ifdef __cplusplus
extern "C" {
#endif

// Mounts the first writable device it finds and creates the save directory.
// Safe to call more than once. Does nothing on platforms other than OGC.
void storage_ogc_init(void);

// True when a writable device was found. Saving is simply unavailable
// otherwise; nothing else in the game changes.
bool storage_ogc_available(void);

// Absolute path for a file name inside the save directory, e.g.
// "sd:/sm64/sm64_save_file.bin". Returns the name unchanged when no device
// mounted, so callers need no special case: the fopen just fails, exactly as
// it did before any of this existed.
//
// The returned string lives in a static buffer that the next call overwrites,
// so never hold two of them at once.
const char *storage_ogc_path(const char *name);

// Whole-file read and write, used for the EEPROM image.
//
// The write is crash-safe: the data goes to a temporary file first and only
// replaces the real one once it is complete on the card. A power cut in the
// middle therefore costs the new save, never the old one -- and the read
// recovers from the temporary if the cut landed in the narrow window where the
// old file was already gone.
bool storage_ogc_read_file(const char *name, void *buf, unsigned int size);
bool storage_ogc_write_file(const char *name, const void *buf, unsigned int size);

#ifdef __cplusplus
}
#endif

#endif
