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
#include <ogc/card.h>
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

#ifndef TARGET_WII
// -- GameCube memory card ----------------------------------------------------
//
// A stock GameCube has a memory card and no SD adapter, so without this the
// console cannot save at all. It is a different API rather than a different
// path: no fopen, writes aligned to the card's sector, and a file that the
// system's memory card manager lists.
//
// Crash safety works differently here too. There is no rename to make an
// update atomic, so the file is two sectors and they are written alternately:
// each carries a sequence number and a checksum, a read takes the valid sector
// with the higher number, and a write always targets the other one. Losing
// power mid-write therefore costs the new save and leaves the previous one
// intact -- the same guarantee as the SD path, reached another way.
//
// Cost on the card: two blocks out of the 59 a standard card holds.

#define CARD_MAGIC   0x534D3634u   // 'SM64'
#define CARD_SECTOR_MAX 8192

struct CardSectorHeader {
    u32 magic;
    u32 seq;
    u32 len;
    u32 sum;
};

static u8 card_work[CARD_WORKAREA_SIZE] __attribute__((aligned(32)));

// A union rather than a byte array plus a cast: the header and the payload
// genuinely share this storage, and saying so in the type removes the cast
// along with any question about whether the alignment holds. CARD_Read and
// CARD_Write need 32 bytes of it anyway.
static union {
    struct CardSectorHeader header;
    u8 bytes[CARD_SECTOR_MAX];
} card_sector __attribute__((aligned(32)));
static bool card_ready;
static s32 card_slot;
static u32 card_sector_size;

static u32 card_checksum(const u8 *p, u32 len) {
    u32 sum = 0x9E3779B9u;
    for (u32 i = 0; i < len; i++) {
        sum = (sum << 1 | sum >> 31) + p[i];
    }
    return sum;
}

// Mount, run one operation, unmount. Holding the card mounted across the whole
// session would keep the EXI channel busy and make swapping cards unsafe, and
// the game only saves at transition points, so the mount cost does not land in
// a gameplay frame.
static bool card_mount(void) {
    return CARD_Mount(card_slot, card_work, NULL) >= 0;
}

static bool card_read_sector(card_file *f, u32 index) {
    return CARD_Read(f, card_sector.bytes, card_sector_size,
                     index * card_sector_size) >= 0;
}

// Returns the sequence number of a sector holding a valid payload, or 0.
static u32 card_sector_seq(card_file *f, u32 index, u32 expect_len) {
    if (!card_read_sector(f, index)) {
        return 0;
    }
    const struct CardSectorHeader *h = &card_sector.header;
    if (h->magic != CARD_MAGIC || h->len != expect_len || h->seq == 0) {
        return 0;
    }
    if (h->sum != card_checksum(card_sector.bytes + sizeof(*h), expect_len)) {
        return 0;
    }
    return h->seq;
}

static bool card_probe(void) {
    // The game code and company show up in the memory card manager. Two ASCII
    // characters and four, exactly, or CARD_Init rejects them.
    CARD_Init("SM64", "01");

    for (s32 slot = CARD_SLOTA; slot <= CARD_SLOTB; slot++) {
        if (CARD_Probe(slot) <= 0) {
            continue;
        }
        card_slot = slot;
        if (!card_mount()) {
            continue;
        }
        u32 sector = 0;
        const bool ok = CARD_GetSectorSize(slot, &sector) >= 0
                     && sector >= 1024 && sector <= CARD_SECTOR_MAX;
        CARD_Unmount(slot);
        if (ok) {
            card_sector_size = sector;
            return true;
        }
    }
    return false;
}

static bool card_read_file(const char *name, void *buf, u32 size) {
    if (!card_ready || size + sizeof(struct CardSectorHeader) > card_sector_size) {
        return false;
    }
    if (!card_mount()) {
        return false;
    }

    bool ok = false;
    card_file f;
    if (CARD_Open(card_slot, name, &f) >= 0) {
        const u32 seq0 = card_sector_seq(&f, 0, size);
        // card_sector_seq leaves the sector it read in the buffer, so read the
        // newer one last and the payload is already there.
        const u32 seq1 = card_sector_seq(&f, 1, size);
        const u32 newest = (seq0 > seq1) ? 0 : 1;
        if (seq0 != 0 || seq1 != 0) {
            if (newest == 0 && card_sector_seq(&f, 0, size) == 0) {
                ok = false;
            } else {
                memcpy(buf, card_sector.bytes + sizeof(struct CardSectorHeader), size);
                ok = true;
            }
        }
        CARD_Close(&f);
    }

    CARD_Unmount(card_slot);
    return ok;
}

static bool card_write_file(const char *name, const void *buf, u32 size) {
    if (!card_ready || size + sizeof(struct CardSectorHeader) > card_sector_size) {
        return false;
    }
    if (!card_mount()) {
        return false;
    }

    bool ok = false;
    card_file f;
    s32 res = CARD_Open(card_slot, name, &f);
    if (res == CARD_ERROR_NOFILE) {
        res = CARD_Create(card_slot, name, card_sector_size * 2, &f);
    }
    if (res >= 0) {
        const u32 seq0 = card_sector_seq(&f, 0, size);
        const u32 seq1 = card_sector_seq(&f, 1, size);
        const u32 newest_seq = (seq0 > seq1) ? seq0 : seq1;
        const u32 target = (seq0 > seq1) ? 1 : 0;   // never overwrite the good one

        memset(card_sector.bytes, 0, card_sector_size);
        struct CardSectorHeader *h = &card_sector.header;
        h->magic = CARD_MAGIC;
        h->seq = newest_seq + 1;
        h->len = size;
        memcpy(card_sector.bytes + sizeof(*h), buf, size);
        h->sum = card_checksum(card_sector.bytes + sizeof(*h), size);

        ok = CARD_Write(&f, card_sector.bytes, card_sector_size,
                        target * card_sector_size) >= 0;
        CARD_Close(&f);
    }

    CARD_Unmount(card_slot);
    return ok;
}
#endif // !TARGET_WII

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
            break;
        }
    }

#ifndef TARGET_WII
    card_ready = card_probe();
#endif
}

bool storage_ogc_available(void) {
#ifndef TARGET_WII
    if (card_ready) {
        return true;
    }
#endif
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

// Which backend owns the save file.
//
// The memory card comes first on GameCube: it is what a stock console has, and
// a save there is visible to the system's card manager. An SD card is only
// present on a modded machine, and someone who has gone to that trouble can
// still force it with -DSTORAGE_OGC_PREFER_FAT=1 -- useful for carrying a save
// to and from the PC build, which the card format cannot do.
//
// Only the save file is routed this way. The config stays on FAT: it is a
// convenience rather than progress, and putting a text file on a memory card
// would cost a block to save keyboard bindings.
#ifndef STORAGE_OGC_PREFER_FAT
#define STORAGE_OGC_PREFER_FAT 0
#endif

bool storage_ogc_read_file(const char *name, void *buf, unsigned int size) {
#if !defined(TARGET_WII) && !STORAGE_OGC_PREFER_FAT
    if (card_ready && card_read_file(name, buf, size)) {
        return true;
    }
#endif
    if (!available) {
#if !defined(TARGET_WII) && STORAGE_OGC_PREFER_FAT
        return card_ready && card_read_file(name, buf, size);
#else
        return false;
#endif
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
#if !defined(TARGET_WII) && !STORAGE_OGC_PREFER_FAT
    if (card_ready && card_write_file(name, buf, size)) {
        return true;
    }
#endif
    if (!available) {
#if !defined(TARGET_WII) && STORAGE_OGC_PREFER_FAT
        return card_ready && card_write_file(name, buf, size);
#else
        return false;
#endif
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
