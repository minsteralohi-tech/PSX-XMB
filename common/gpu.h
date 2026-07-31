/*
 * ps1-bare-metal - (C) 2023-2025 spicyjpeg
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#pragma once

#include <stdint.h>
#include "ps1/gpucmd.h"

// Size (in 32-bit words) of each frame's GPU DMA command buffer. The menu text
// alone can use well over 1500 words (every glyph is a ~5-word packet), and the
// additive wave backgrounds add several hundred more, so the original 2048 was
// only barely enough for the lightest theme and overflowed on heavier ones -
// silently corrupting memory in release builds (no assert), which showed up as
// DuckStation crashes and full console resets on real hardware. 4096 gives
// comfortable headroom for the menu + any background + the upcoming icon sheet.
#define GPU_CHAIN_BUFFER_SIZE 4096

typedef struct {
	uint32_t data[GPU_CHAIN_BUFFER_SIZE];
	uint32_t *nextPacket;
} GPUDMAChain;

typedef struct {
	uint8_t  u, v;
	uint16_t width, height;
	uint16_t page, clut;
} TextureInfo;

#ifdef __cplusplus
extern "C" {
#endif

void setupGPU(GP1VideoMode mode, int width, int height);
void waitForGP0Ready(void);
void waitForGPUDMADone(void);
void waitForVSync(void);

void sendGPULinkedList(const void *data);
void sendVRAMData(
	const void *data,
	int        x,
	int        y,
	int        width,
	int        height
);
void receiveVRAMData(
	void *data,
	int  x,
	int  y,
	int  width,
	int  height
);
uint32_t *allocateGP0Packet(GPUDMAChain *chain, int numCommands);

void uploadTexture(
	TextureInfo *info,
	const void  *data,
	int         x,
	int         y,
	int         width,
	int         height
);
void uploadIndexedTexture(
	TextureInfo   *info,
	const void    *image,
	const void    *palette,
	int           imageX,
	int           imageY,
	int           paletteX,
	int           paletteY,
	int           width,
	int           height,
	GP0ColorDepth colorDepth
);

#ifdef __cplusplus
}
#endif
