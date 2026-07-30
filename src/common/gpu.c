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

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "common/gpu.h"
#include "ps1/gpucmd.h"
#include "ps1/registers.h"

#define DMA_MAX_CHUNK_SIZE 16

void setupGPU(GP1VideoMode mode, int width, int height) {
	int x = 0x760;
	int y = (mode == GP1_MODE_PAL) ? 0xa3 : 0x88;

	GP1HorizontalRes horizontalRes = GP1_HRES_320;
	GP1VerticalRes   verticalRes   = GP1_VRES_256;

	int offsetX = (width  * gp1_clockMultiplierH(horizontalRes)) / 2;
	int offsetY = (height / gp1_clockDividerV(verticalRes))      / 2;

	GPU_GP1 = gp1_resetGPU();
	GPU_GP1 = gp1_fbRangeH(x - offsetX, x + offsetX);
	GPU_GP1 = gp1_fbRangeV(y - offsetY, y + offsetY);
	GPU_GP1 = gp1_fbMode(
		horizontalRes,
		verticalRes,
		mode,
		false,
		GP1_COLOR_16BPP
	);
	GPU_GP1 = gp1_dispBlank(false);

	DMA_DPCR         |= DMA_DPCR_CH_ENABLE(DMA_GPU);
	DMA_CHCR(DMA_GPU) = 0;
}

void waitForGP0Ready(void) {
	while (!(GPU_GP1 & GP1_STAT_CMD_READY))
		__asm__ volatile("");
}

void waitForGPUDMADone(void) {
	while (DMA_CHCR(DMA_GPU) & DMA_CHCR_ENABLE)
		__asm__ volatile("");
}

void waitForVSync(void) {
	while (!(IRQ_STAT & (1 << IRQ_VSYNC)))
		__asm__ volatile("");

	IRQ_STAT = ~(1 << IRQ_VSYNC);
}

void sendGPULinkedList(const void *data) {
	waitForGPUDMADone();
	assert(!((uint32_t) data % 4));

	GPU_GP1 = gp1_dmaRequestMode(GP1_DREQ_GP0_WRITE);

	DMA_MADR(DMA_GPU) = (uint32_t) data;
	DMA_CHCR(DMA_GPU) = 0
		| DMA_CHCR_WRITE
		| DMA_CHCR_MODE_LIST
		| DMA_CHCR_ENABLE;
}

void sendVRAMData(
	const void *data,
	int        x,
	int        y,
	int        width,
	int        height
) {
	waitForGPUDMADone();
	assert(!((uint32_t) data % 4));

	size_t length = (width * height + 1) / 2;
	size_t chunkSize, numChunks;

	if (length < DMA_MAX_CHUNK_SIZE) {
		chunkSize = length;
		numChunks = 1;
	} else {
		chunkSize = DMA_MAX_CHUNK_SIZE;
		numChunks = length / DMA_MAX_CHUNK_SIZE;

		assert(!(length % DMA_MAX_CHUNK_SIZE));
	}

	GPU_GP1 = gp1_dmaRequestMode(GP1_DREQ_NONE);

	waitForGP0Ready();
	GPU_GP0 = gp0_vramWrite();
	GPU_GP0 = gp0_xy(x, y);
	GPU_GP0 = gp0_xy(width, height);

	GPU_GP1 = gp1_dmaRequestMode(GP1_DREQ_GP0_WRITE);

	DMA_MADR(DMA_GPU) = (uint32_t) data;
	DMA_BCR (DMA_GPU) = chunkSize | (numChunks << 16);
	DMA_CHCR(DMA_GPU) = 0
		| DMA_CHCR_WRITE
		| DMA_CHCR_MODE_SLICE
		| DMA_CHCR_ENABLE;
}

void receiveVRAMData(
	void *data,
	int  x,
	int  y,
	int  width,
	int  height
) {
	waitForGPUDMADone();
	assert(!((uint32_t) data % 4));

	size_t length = (width * height + 1) / 2;
	size_t chunkSize, numChunks;

	if (length < DMA_MAX_CHUNK_SIZE) {
		chunkSize = length;
		numChunks = 1;
	} else {
		chunkSize = DMA_MAX_CHUNK_SIZE;
		numChunks = length / DMA_MAX_CHUNK_SIZE;

		assert(!(length % DMA_MAX_CHUNK_SIZE));
	}

	GPU_GP1 = gp1_dmaRequestMode(GP1_DREQ_NONE);

	waitForGP0Ready();
	GPU_GP0 = gp0_vramRead();
	GPU_GP0 = gp0_xy(x, y);
	GPU_GP0 = gp0_xy(width, height);

	GPU_GP1 = gp1_dmaRequestMode(GP1_DREQ_GP0_READ);

	DMA_MADR(DMA_GPU) = (uint32_t) data;
	DMA_BCR (DMA_GPU) = chunkSize | (numChunks << 16);
	DMA_CHCR(DMA_GPU) = 0
		| DMA_CHCR_READ
		| DMA_CHCR_MODE_SLICE
		| DMA_CHCR_ENABLE;
}

uint32_t *allocateGP0Packet(GPUDMAChain *chain, int numCommands) {
	assert((numCommands >= 0) && (numCommands <= DMA_MAX_CHUNK_SIZE));

	// Real, always-compiled bounds check - the assert() a few lines down
	// used to be the ONLY protection here, and this project builds with
	// -DNDEBUG (see CMakeLists.txt), which strips every assert() out of
	// the release build entirely. That means there was actually no bounds
	// checking at all in what ships: a theme (or a theme combined with a
	// particularly text-heavy screen on top of it) that asked for more
	// than GPU_CHAIN_BUFFER_SIZE words in one frame would silently write
	// past chain->data[] into whatever memory happens to sit right after
	// it - corrupting it. That's the actual mechanism behind more than
	// one "random crash depending on what else is on screen" bug this
	// project has hit.
	//
	// If a caller is about to overflow, redirect this one packet into a
	// small scratch buffer that isn't linked into the real DMA chain
	// (chain->nextPacket is deliberately NOT advanced in this branch) -
	// the packet is silently dropped rather than drawn, so the frame ends
	// up with something visibly missing or wrong near the point it ran
	// out of room, but the program does not corrupt unrelated memory or
	// crash. A dropped primitive is a small, visible, recoverable bug;
	// memory corruption is neither small nor visible until it's already
	// too late to trace back to its cause - see everything above this
	// comment for how much time that's cost already.
	static uint32_t overflowScratch[DMA_MAX_CHUNK_SIZE + 2];
	if (chain->nextPacket + numCommands + 1 >= &chain->data[GPU_CHAIN_BUFFER_SIZE]) {
		uint32_t *ptr = overflowScratch;
		ptr[0] = gp0_tag(numCommands, &overflowScratch[numCommands + 1]);
		return &ptr[1];
	}

	uint32_t *ptr      = chain->nextPacket;
	chain->nextPacket += numCommands + 1;

	*ptr = gp0_tag(numCommands, chain->nextPacket);
	assert(chain->nextPacket < &(chain->data)[GPU_CHAIN_BUFFER_SIZE]);

	return &ptr[1];
}

void uploadTexture(
	TextureInfo *info,
	const void  *data,
	int         x,
	int         y,
	int         width,
	int         height
) {
	assert((width <= 256) && (height <= 256));

	sendVRAMData(data, x, y, width, height);
	waitForGPUDMADone();
	GPU_GP0 = gp0_flushCache();

	info->page   = gp0_page(
		x /  64,
		y / 256,
		GP0_BLEND_SEMITRANS,
		GP0_COLOR_16BPP
	);
	info->clut   = 0;
	info->u      = (uint8_t)  (x %  64);
	info->v      = (uint8_t)  (y % 256);
	info->width  = (uint16_t) width;
	info->height = (uint16_t) height;
}

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
) {
	assert((width <= 256) && (height <= 256));

	int numColors    = (colorDepth == GP0_COLOR_8BPP) ? 256 : 16;
	int widthDivider = (colorDepth == GP0_COLOR_8BPP) ?   2 :  4;

	assert(!(paletteX % 16) && ((paletteX + numColors) <= 1024));

	sendVRAMData(image, imageX, imageY, width / widthDivider, height);
	waitForGPUDMADone();
	sendVRAMData(palette, paletteX, paletteY, numColors, 1);
	waitForGPUDMADone();
	GPU_GP0 = gp0_flushCache();

	info->page   = gp0_page(
		imageX /  64,
		imageY / 256,
		GP0_BLEND_SEMITRANS,
		colorDepth
	);
	info->clut   = gp0_clut(paletteX / 16, paletteY);
	info->u      = (uint8_t)  ((imageX %  64) * widthDivider);
	info->v      = (uint8_t)   (imageY % 256);
	info->width  = (uint16_t) width;
	info->height = (uint16_t) height;
}
