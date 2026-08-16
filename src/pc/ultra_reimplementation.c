#include <stdio.h>
#include <string.h>
#include "lib/src/libultra_internal.h"
#include "macros.h"

#ifdef TARGET_WEB
#include <emscripten.h>
#endif

#ifdef TARGET_OGC
// No libogc header here on purpose: <ogc/lwp_watchdog.h> reaches ogcsys.h,
// which pulls ogc/gx.h and ogc/gu.h, whose Vtx and Mtx clash with the ones
// PR/gbi.h declares through ultra64.h above. gfx_ogc.h is libogc-free and
// exposes the Time Base through a real (non-inline) function.
#include "gfx/gfx_ogc.h"
// Same rule: storage_ogc.h is libogc-free, so the save path can be resolved
// from here without dragging <fat.h> and its dependencies into a translation
// unit that already has the GBI.
#include "storage_ogc.h"
#endif

// Kept identical to the PC build's name and format -- a raw 512-byte EEPROM
// dump, big-endian on both sides -- so a save can be carried between them.
#define SAVE_FILE_NAME "sm64_save_file.bin"
#define SAVE_FILE_PATH SAVE_FILE_NAME

extern OSMgrArgs piMgrArgs;

u64 osClockRate = 62500000;

s32 osPiStartDma(UNUSED OSIoMesg *mb, UNUSED s32 priority, UNUSED s32 direction,
                 uintptr_t devAddr, void *vAddr, size_t nbytes,
                 UNUSED OSMesgQueue *mq) {
    memcpy(vAddr, (const void *) devAddr, nbytes);
    return 0;
}

void osCreateMesgQueue(OSMesgQueue *mq, OSMesg *msgBuf, s32 count) {
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msgBuf;
    return;
}

void osSetEventMesg(UNUSED OSEvent e, UNUSED OSMesgQueue *mq, UNUSED OSMesg msg) {
}
s32 osJamMesg(UNUSED OSMesgQueue *mq, UNUSED OSMesg msg, UNUSED s32 flag) {
    return 0;
}
s32 osSendMesg(UNUSED OSMesgQueue *mq, UNUSED OSMesg msg, UNUSED s32 flag) {
#if defined(VERSION_EU) || defined(VERSION_SH)
    s32 index;
    if (mq->validCount >= mq->msgCount) {
        return -1;
    }
    index = (mq->first + mq->validCount) % mq->msgCount;
    mq->msg[index] = msg;
    mq->validCount++;
#endif
    return 0;
}
s32 osRecvMesg(UNUSED OSMesgQueue *mq, UNUSED OSMesg *msg, UNUSED s32 flag) {
#if defined(VERSION_EU) || defined(VERSION_SH)
    if (mq->validCount == 0) {
        return -1;
    }
    if (msg != NULL) {
        *msg = *(mq->first + mq->msg);
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
#endif
    return 0;
}

uintptr_t osVirtualToPhysical(void *addr) {
    return (uintptr_t) addr;
}

void osCreateViManager(UNUSED OSPri pri) {
}
void osViSetMode(UNUSED OSViMode *mode) {
}
void osViSetEvent(UNUSED OSMesgQueue *mq, UNUSED OSMesg msg, UNUSED u32 retraceCount) {
}
void osViBlack(UNUSED u8 active) {
}
void osViSetSpecialFeatures(UNUSED u32 func) {
}
void osViSwapBuffer(UNUSED void *vaddr) {
}

OSTime osGetTime(void) {
#ifdef TARGET_OGC
    // Time Base Register, monotonic since boot. Beats the stock `return 0`,
    // though the tick rate is the console's, not the N64's osClockRate.
    return (OSTime) gfx_ogc_get_ticks();
#else
    return 0;
#endif
}

void osWritebackDCacheAll(void) {
}

void osWritebackDCache(UNUSED void *a, UNUSED size_t b) {
}

void osInvalDCache(UNUSED void *a, UNUSED size_t b) {
}

u32 osGetCount(void) {
    static u32 counter;
    return counter++;
}

s32 osAiSetFrequency(u32 freq) {
    u32 a1;
    s32 a2;
    u32 D_8033491C;

#ifdef VERSION_EU
    D_8033491C = 0x02E6025C;
#else
    D_8033491C = 0x02E6D354;
#endif

    a1 = D_8033491C / (float) freq + .5f;

    if (a1 < 0x84) {
        return -1;
    }

    a2 = (a1 / 66) & 0xff;
    if (a2 > 16) {
        a2 = 16;
    }

    return D_8033491C / (s32) a1;
}

s32 osEepromProbe(UNUSED OSMesgQueue *mq) {
    return 1;
}

s32 osEepromLongRead(UNUSED OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes) {
    u8 content[512];
    s32 ret = -1;

#ifdef TARGET_WEB
    if (EM_ASM_INT({
        var s = localStorage.sm64_save_file;
        if (s && s.length === 684) {
            try {
                var binary = atob(s);
                if (binary.length === 512) {
                    for (var i = 0; i < 512; i++) {
                        HEAPU8[$0 + i] = binary.charCodeAt(i);
                    }
                    return 1;
                }
            } catch (e) {
            }
        }
        return 0;
    }, content)) {
        memcpy(buffer, content + address * 8, nbytes);
        ret = 0;
    }
#elif defined(TARGET_OGC)
    // Goes through storage_ogc so a save interrupted by a power cut can be
    // recovered from the temporary file it was being written to.
    if (storage_ogc_read_file(SAVE_FILE_NAME, content, 512)) {
        memcpy(buffer, content + address * 8, nbytes);
        ret = 0;
    }
#else
    FILE *fp = fopen(SAVE_FILE_PATH, "rb");
    if (fp == NULL) {
        return -1;
    }
    if (fread(content, 1, 512, fp) == 512) {
        memcpy(buffer, content + address * 8, nbytes);
        ret = 0;
    }
    fclose(fp);
#endif
    return ret;
}

s32 osEepromLongWrite(UNUSED OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes) {
    u8 content[512] = {0};
    if (address != 0 || nbytes != 512) {
        osEepromLongRead(mq, 0, content, 512);
    }
    memcpy(content + address * 8, buffer, nbytes);

#ifdef TARGET_WEB
    EM_ASM({
        var str = "";
        for (var i = 0; i < 512; i++) {
            str += String.fromCharCode(HEAPU8[$0 + i]);
        }
        localStorage.sm64_save_file = btoa(str);
    }, content);
    s32 ret = 0;
#elif defined(TARGET_OGC)
    s32 ret = storage_ogc_write_file(SAVE_FILE_NAME, content, 512) ? 0 : -1;
#else
    FILE* fp = fopen(SAVE_FILE_PATH, "wb");
    if (fp == NULL) {
        return -1;
    }
    s32 ret = fwrite(content, 1, 512, fp) == 512 ? 0 : -1;
    fclose(fp);
#endif
    return ret;
}

s32 gNumVblanks;

s32 osMotorInit(UNUSED OSMesgQueue *mq, UNUSED void *pfs, UNUSED int channel) {
    return 0;
}

s32 osMotorStart(UNUSED void *pfs) {
    return 0;
}

s32 osMotorStop(UNUSED void *pfs) {
    return 0;
}

OSPiHandle *osCartRomInit(void) {
    static OSPiHandle handle;
    return &handle;
}

OSPiHandle *osDriveRomInit(void) {
    static OSPiHandle handle;
    return &handle;
}

s32 osEPiStartDma(UNUSED OSPiHandle *pihandle, OSIoMesg *mb, UNUSED s32 direction) {
    memcpy(mb->dramAddr, (const void *) mb->devAddr, mb->size);
    osSendMesg(mb->hdr.retQueue, mb, OS_MESG_NOBLOCK);
}
