#ifndef AUDIO_OGC_H
#define AUDIO_OGC_H

#ifdef TARGET_OGC

#include "audio_api.h"

extern struct AudioAPI audio_ogc;

// Silences the hardware and stops the DMA engine, for the shutdown path.
//
// It is not part of AudioAPI: that interface is shared with every other
// platform, and none of the others needs one -- their process simply ends and
// the OS reclaims the device. Here there is no OS to do it. The DMA engine
// keeps walking the buffer after the game has stopped feeding it, so whatever
// loads next inherits a running transfer into memory it is about to use.
//
// Safe to call more than once, and safe to call when audio never initialised.
void audio_ogc_stop(void);

#endif // TARGET_OGC

#endif
