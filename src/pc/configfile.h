#ifndef CONFIGFILE_H
#define CONFIGFILE_H

extern bool         configFullscreen;
extern bool         configWidescreen;
// Overscan margin in framebuffer pixels, inset on every edge. Televisions crop
// 5-10% of the picture; this pulls the whole image inward so nothing lands off
// the tube. Zero by default, which draws exactly as before -- a value that
// suits one set is wrong on the next, so it has to be the player's to choose.
extern unsigned int configOverscan;
extern unsigned int configKeyA;
extern unsigned int configKeyB;
extern unsigned int configKeyStart;
extern unsigned int configKeyR;
extern unsigned int configKeyZ;
extern unsigned int configKeyCUp;
extern unsigned int configKeyCDown;
extern unsigned int configKeyCLeft;
extern unsigned int configKeyCRight;
extern unsigned int configKeyStickUp;
extern unsigned int configKeyStickDown;
extern unsigned int configKeyStickLeft;
extern unsigned int configKeyStickRight;

void configfile_load(const char *filename);
void configfile_save(const char *filename);

#endif
