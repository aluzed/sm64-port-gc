#ifdef TARGET_OGC

// Persistent storage on GameCube and Wii.
//
// libfat was linked from the start but fatInitDefault() was never called, so
// no device was ever mounted and every fopen failed: no save file, no config.
// The game runs fine that way, which is exactly why it went unnoticed.
//
// The working directory is not something to rely on here. Depending on the
// loader a homebrew binary starts with the CWD set to the device root, to the
// directory it was launched from, or to nothing at all, so a relative
// "sm64_save_file.bin" can land anywhere or nowhere. Every path this module
// hands out is absolute.

#include <fat.h>
#ifndef TARGET_WII
#include <sdcard/gcsd.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "storage_ogc.h"

#define SAVE_DIR "sm64"

// Search order. fatInitDefault() mounts whatever it can find; these are the
// names those mounts take.
//
//   Wii:       an SD card in the front slot, then a USB mass storage device.
//   GameCube:  Serial Port 2 first -- that is where an SD2SP2 lives, and it is
//              the one slot that cannot be occupied by a memory card -- then an
//              SD Gecko in either memory card slot.
//
// port2 is not in the table fatInitDefault() walks, so it has to be mounted by
// hand; see storage_ogc_init. Getting that wrong means an SD2SP2 is simply
// never found, with no error anywhere.
//
// A real GameCube memory card is *not* a FAT volume. Probing carda on one fails
// harmlessly, but it will never be found here: it needs the CARD API, which is
// a separate job with its own failure modes (block allocation, banner and icon,
// 8 KB granularity). See STORY-015.
static const char *const devices[] = {
#ifdef TARGET_WII
    "sd", "usb", "carda", "cardb",
#else
    "port2", "carda", "cardb", "sd", "usb",
#endif
};

static bool initialised;
static bool available;
static char base_dir[64];
static char path_buf[128];

static bool try_device(const char *dev) {
    char dir[64];
    snprintf(dir, sizeof(dir), "%s:/" SAVE_DIR, dev);

    // mkdir tells us three things at once: the device is mounted, it is
    // writable, and the directory now exists. EEXIST is success.
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        return false;
    }

    // mkdir can succeed on a card that is write protected or that fails on the
    // first real write, so prove the directory is usable by writing to it.
    char probe[128];
    snprintf(probe, sizeof(probe), "%s/.probe", dir);
    FILE *fp = fopen(probe, "wb");
    if (fp == NULL) {
        return false;
    }
    const bool ok = fwrite("ok", 1, 2, fp) == 2;
    fclose(fp);
    remove(probe);
    if (!ok) {
        return false;
    }

    snprintf(base_dir, sizeof(base_dir), "%s", dir);
    return true;
}

void storage_ogc_init(void) {
    if (initialised) {
        return;
    }
    initialised = true;

    // Mounts every device it recognises and picks a default. It is slow enough
    // to be worth calling exactly once, and it is why this function is guarded.
    // Its return value is not a verdict: it reports whether a *default* device
    // was set, and an SD2SP2 mounted below is perfectly usable without one.
    fatInitDefault();

#ifndef TARGET_WII
    // Serial Port 2. libfat's automatic scan only covers the two memory card
    // slots, so an SD2SP2 has to be mounted explicitly or it stays invisible.
    fatMountSimple("port2", &__io_gcsd2);
#endif

    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        if (try_device(devices[i])) {
            available = true;
            return;
        }
    }
}

bool storage_ogc_available(void) {
    return available;
}

const char *storage_ogc_path(const char *name) {
    if (!available) {
        return name;
    }
    snprintf(path_buf, sizeof(path_buf), "%s/%s", base_dir, name);
    return path_buf;
}

static bool read_whole(const char *path, void *buf, unsigned int size) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return false;
    }
    const bool ok = fread(buf, 1, size, fp) == size;
    fclose(fp);
    return ok;
}

bool storage_ogc_read_file(const char *name, void *buf, unsigned int size) {
    if (!available) {
        return false;
    }

    char real[160], temp[160];
    snprintf(real, sizeof(real), "%s/%s", base_dir, name);
    snprintf(temp, sizeof(temp), "%s/%s.tmp", base_dir, name);

    if (read_whole(real, buf, size)) {
        return true;
    }
    // The real file is gone but a complete temporary is not: the console lost
    // power inside the rename window below. The temporary is the newer save,
    // so recovering from it is both safe and correct.
    return read_whole(temp, buf, size);
}

bool storage_ogc_write_file(const char *name, const void *buf, unsigned int size) {
    if (!available) {
        return false;
    }

    char real[160], temp[160];
    snprintf(real, sizeof(real), "%s/%s", base_dir, name);
    snprintf(temp, sizeof(temp), "%s/%s.tmp", base_dir, name);

    // Write the whole thing somewhere else first. Truncating the real save and
    // then losing power is what costs a player their stars, and it is entirely
    // avoidable.
    FILE *fp = fopen(temp, "wb");
    if (fp == NULL) {
        return false;
    }
    bool ok = fwrite(buf, 1, size, fp) == size;
    if (fflush(fp) != 0) {
        ok = false;
    }
    if (fclose(fp) != 0) {
        ok = false;
    }
    if (!ok) {
        remove(temp);
        return false;
    }

    // libfat's rename does not replace an existing file, so the old one has to
    // go first. That leaves a window with no real file -- which is exactly what
    // the recovery in storage_ogc_read_file covers.
    remove(real);
    if (rename(temp, real) != 0) {
        return false;
    }
    return true;
}

#endif // TARGET_OGC
