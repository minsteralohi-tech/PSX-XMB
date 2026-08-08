#ifndef MODELTEX_DATA_H
#define MODELTEX_DATA_H

#include <stdint.h>

#define MODELTEX_WIDTH  128
#define MODELTEX_HEIGHT 128

/*
 * Raw PS1 16bpp (1 STP + 5B + 5G + 5R) texture data, row-major.
 *
 * The PS one's own texture, reduced from the 1024x1024 sheet embedded in
 * assets/ps_one_pixel.glb. The data lives in modeltex.c because two screens
 * draw it; see that file. tools/glb2console.py re-derives it on every run and
 * warns if this stops matching the GLB, so the two cannot drift apart
 * unnoticed.
 */
extern const uint16_t modeltexTextureData[MODELTEX_WIDTH * MODELTEX_HEIGHT];

#endif
