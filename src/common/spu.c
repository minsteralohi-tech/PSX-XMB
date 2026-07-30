/*
 * ps1-bare-metal - (C) 2023-2026 spicyjpeg
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
#include <stddef.h>
#include <stdint.h>
#include "common/spu.h"
#include "ps1/registers.h"

#define DMA_MAX_CHUNK_SIZE 16

static void delayMicroseconds(int time) {
	time = ((time * 271) + 4) / 8;

	__asm__ volatile(
		".set push\n"
		".set noreorder\n"
		"bgtz  %0, .\n"
		"addiu %0, -2\n"
		".set pop\n"
		: "+r"(time)
	);
}

void initSPU(void) {
	BIU_DEV4_CTRL = 0
		| ( 1 << 0) // Write delay
		| (14 << 4) // Read delay
		| BIU_CTRL_RECOVERY
		| BIU_CTRL_WIDTH_16
		| BIU_CTRL_AUTO_INCR
		| (9 << 16) // Number of address lines
		| (2 << 24) // DMA read/write delay (required for SPU RAM readback)
		| BIU_CTRL_DMA_DELAY;

	SPU_CTRL = 0;

	while (SPU_STAT & 0x07ff)
		__asm__ volatile("");

	SPU_MVOLL    = SPU_MAX_VOLUME;
	SPU_MVOLR    = SPU_MAX_VOLUME;
	SPU_EVOLL    = 0;
	SPU_EVOLR    = 0;
	SPU_ESA      = (SPU_RAM_SIZE - 16) / SPU_RAM_ADDR_UNIT;
	SPU_RAM_CTRL = 4;
	SPU_CTRL     = 0
		| SPU_CTRL_XFER_NONE
		| SPU_CTRL_DAC_ENABLE
		| SPU_CTRL_ENABLE;

	stopAllSPUChannels();

	DMA_DPCR         |= DMA_DPCR_CH_ENABLE(DMA_SPU);
	DMA_CHCR(DMA_SPU) = 0;
}

void waitForSPUDMADone(void) {
	while (DMA_CHCR(DMA_SPU) & DMA_CHCR_ENABLE)
		__asm__ volatile("");

	// A delay is required here in order to allow the SPU to flush its transfer
	// FIFO to SPU RAM. This takes around 30 us when the FIFO is full.
	delayMicroseconds(35);
}

void stopAllSPUChannels(void) {
	// Reset all channels and point them to the beginning of SPU RAM. This is
	// not strictly required but useful when using the SPU's interrupt feature,
	// as "stopped" channels will actually keep reading samples from memory and
	// may accidentally trigger an IRQ.
	// NOTE: doing this is technically invalid as the first 4 KB of SPU RAM
	// contain PCM capture buffers rather than ADPCM samples, but it does not
	// matter as we're also setting the pitch to zero.
	for (int i = 0; i < SPU_NUM_CHANNELS; i++) {
		SPU_CH_VOLL (i) = 0;
		SPU_CH_VOLR (i) = 0;
		SPU_CH_PITCH(i) = 0;
		SPU_CH_SSA  (i) = 0;
		SPU_CH_ADSR1(i) = 0;
		SPU_CH_ADSR2(i) = 0;
		SPU_CH_LSAX (i) = 0;
	}

	SPU_PMON0 = 0;
	SPU_PMON1 = 0;
	SPU_NON0  = 0;
	SPU_NON1  = 0;
	SPU_EON0  = 0;
	SPU_EON1  = 0;

	// Key on all channels to force the current playback address to be updated,
	// then key them off again.
	SPU_KON0  = 0xffff;
	SPU_KON1  = 0x00ff;
	delayMicroseconds(1);
	SPU_KOFF0 = 0xffff;
	SPU_KOFF1 = 0x00ff;
}

int findFreeSPUChannel(void) {
	for (int i = 0; i < SPU_NUM_CHANNELS; i++) {
		if (!SPU_CH_ENVX(i))
			return i;
	}

	return -1;
}

void sendSPURAMData(const void *data, int offset, size_t length) {
	waitForSPUDMADone();
	assert(!((uint32_t) data % 4));
	assert(!(offset % SPU_RAM_ADDR_UNIT));

	length = (length + 3) / 4;
	size_t chunkSize, numChunks;

	if (length < DMA_MAX_CHUNK_SIZE) {
		chunkSize = length;
		numChunks = 1;
	} else {
		chunkSize = DMA_MAX_CHUNK_SIZE;
		numChunks = length / DMA_MAX_CHUNK_SIZE;

		assert(!(length % DMA_MAX_CHUNK_SIZE));
	}

	uint16_t ctrl = SPU_CTRL & ~SPU_CTRL_XFER_BITMASK;
	SPU_CTRL      = ctrl;

	while ((SPU_STAT & SPU_STAT_XFER_BITMASK) != SPU_STAT_XFER_NONE)
		__asm__ volatile("");

	SPU_TSA  = offset / SPU_RAM_ADDR_UNIT;
	SPU_CTRL = ctrl | SPU_CTRL_XFER_DMA_WRITE;

	while ((SPU_STAT & SPU_STAT_XFER_BITMASK) != SPU_STAT_XFER_DMA_WRITE)
		__asm__ volatile("");

	DMA_MADR(DMA_SPU) = (uint32_t) data;
	DMA_BCR (DMA_SPU) = chunkSize | (numChunks << 16);
	DMA_CHCR(DMA_SPU) = 0
		| DMA_CHCR_WRITE
		| DMA_CHCR_MODE_SLICE
		| DMA_CHCR_ENABLE;
}

void receiveSPURAMData(void *data, int offset, size_t length) {
	waitForSPUDMADone();
	assert(!((uint32_t) data % 4));
	assert(!(offset % SPU_RAM_ADDR_UNIT));

	length = (length + 3) / 4;
	size_t chunkSize, numChunks;

	if (length < DMA_MAX_CHUNK_SIZE) {
		chunkSize = length;
		numChunks = 1;
	} else {
		chunkSize = DMA_MAX_CHUNK_SIZE;
		numChunks = length / DMA_MAX_CHUNK_SIZE;

		assert(!(length % DMA_MAX_CHUNK_SIZE));
	}

	uint16_t ctrl = SPU_CTRL & ~SPU_CTRL_XFER_BITMASK;
	SPU_CTRL      = ctrl;

	while ((SPU_STAT & SPU_STAT_XFER_BITMASK) != SPU_STAT_XFER_NONE)
		__asm__ volatile("");

	SPU_TSA  = offset / SPU_RAM_ADDR_UNIT;
	SPU_CTRL = ctrl | SPU_CTRL_XFER_DMA_READ;

	while ((SPU_STAT & SPU_STAT_XFER_BITMASK) != SPU_STAT_XFER_DMA_READ)
		__asm__ volatile("");

	DMA_MADR(DMA_SPU) = (uint32_t) data;
	DMA_BCR (DMA_SPU) = chunkSize | (numChunks << 16);
	DMA_CHCR(DMA_SPU) = 0
		| DMA_CHCR_READ
		| DMA_CHCR_MODE_SLICE
		| DMA_CHCR_ENABLE;
}
