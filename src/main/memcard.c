/*
 * PSX-iTests - Memory Card manager (Phase 1: detection)
 *
 * This is intentionally scoped small for a first pass: just detecting
 * whether a card is present in each slot, using the Get ID command
 * (0x53). Directory reading (listing save files), icon extraction, and
 * any write/delete functionality are all deliberately NOT implemented
 * yet - matching this project's established pattern of building
 * hardware-facing features incrementally and getting each piece
 * confirmed working before adding the next.
 *
 * Protocol confirmed directly from psx-spx's own memory card command
 * tables (Controllers and Memory Cards page), not derived from a
 * secondhand summary:
 *
 *   Send 81h -> memory card address select (vs 01h for controllers)
 *   Send 53h ('S') -> Get ID command, receive FLAG byte
 *   Send 00h x8    -> receive ID1(5Ah), ID2(5Dh), ACK1(5Ch), ACK2(5Dh),
 *                     then 04h,00h,00h,80h (Sony-card-specific info -
 *                     psx-spx notes third-party cards may not return
 *                     this part correctly, so presence detection here
 *                     relies only on ID1/ID2, not the full sequence)
 *
 * Reuses selectControllerPort() from common/sio0.c (a simple, non-timing-
 * critical port selector), but uses a fully self-contained low-level
 * SIO0 exchange implementation rather than sio0.c's own
 * exchangeSIO0Packet() - see the comment above that implementation below
 * for why.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "common/sio0.h"
#include "main/defs.h"
#include "main/font.h"
#include "main/mainmenu.h"
#include "main/sound.h"
#include "main/xmb_bg.h"
#include "main/memcard.h"
#include "ps1/gpucmd.h"
#include "ps1/registers.h"

/* ---- Self-contained SIO0 exchange (not shared with sio0.c) ----
 *
 * This replicates common/sio0.c's exchangeSIO0Packet() logic exactly,
 * rather than reusing or modifying it - matching this project's
 * established discipline of keeping hardware-facing features
 * self-contained, never touching shared files that other already-proven
 * code (the pad tester) depends on. An earlier version of this added an
 * optional-timeout variant directly to sio0.c; that risked the shared,
 * already-working code path even though the change was intended to be
 * behavior-preserving, and real hardware detection broke immediately
 * after - reverted rather than risk debugging a regression in code the
 * pad tester also depends on.
 *
 * The one deliberate difference from the original: a much longer
 * acknowledge timeout, hardcoded in from the start. psx-spx documents
 * that original Sony memory cards have an extra ~31000-cycle (~915us)
 * delay after the 7th byte of the Read command specifically - the
 * default 120us timeout used elsewhere is far too short for that, which
 * is why reads were failing on real hardware while working fine on
 * DuckStation (which doesn't seem to model that delay).
 */

#define MEMCARD_DTR_DELAY     60
#define MEMCARD_TIMEOUT_SHORT 120  // matches the original, proven DSR_TIMEOUT
#define MEMCARD_TIMEOUT_READ  3000 // only Read Sector needs this, per psx-spx

static void memcardDelay(int time) {
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

static bool memcardWaitForAcknowledge(int timeout) {
	for (; timeout > 0; timeout -= 10) {
		if (IRQ_STAT & (1 << IRQ_SIO0)) {
			IRQ_STAT     = ~(1 << IRQ_SIO0);
			SIO_CTRL(0) |= SIO_CTRL_ACKNOWLEDGE;

			// Small settling delay before proceeding - the original
			// code this is based on has an acknowledged "FIXME: not
			// 100% reliable due to metastability issues" on this exact
			// edge-detection. Memory card reads are a much longer, more
			// demanding exchange (139 bytes vs. a controller poll's 4-8)
			// so are more likely to actually hit that weak point. This
			// costs nothing here since a one-time sector read isn't
			// performance-critical.
			memcardDelay(20);

			return true;
		}

		memcardDelay(10);
	}

	return false;
}

static uint8_t memcardExchangeByte(uint8_t value) {
	while (!(SIO_STAT(0) & SIO_STAT_TX_NOT_FULL))
		__asm__ volatile("");

	SIO_DATA(0) = value;

	while (!(SIO_STAT(0) & SIO_STAT_RX_NOT_EMPTY))
		__asm__ volatile("");

	return SIO_DATA(0);
}

static size_t memcardExchangePacket(
	const uint8_t *request,
	uint8_t       *response,
	size_t        reqLength,
	size_t        maxRespLength,
	int           timeout
) {
	IRQ_STAT     = ~(1 << IRQ_SIO0);
	SIO_CTRL(0) |= SIO_CTRL_DTR | SIO_CTRL_ACKNOWLEDGE;
	memcardDelay(MEMCARD_DTR_DELAY);

	size_t respLength = 0;

	SIO_DATA(0) = SIO0_ADDR_MEMORY_CARD;

	if (memcardWaitForAcknowledge(timeout)) {
		while (SIO_STAT(0) & SIO_STAT_RX_NOT_EMPTY)
			SIO_DATA(0);

		while (respLength < maxRespLength) {
			if (reqLength > 0) {
				*(response++) = memcardExchangeByte(*(request++));
				reqLength--;
			} else {
				*(response++) = memcardExchangeByte(0);
			}

			respLength++;

			if (!memcardWaitForAcknowledge(timeout))
				break;
		}
	}

	memcardDelay(MEMCARD_DTR_DELAY);
	SIO_CTRL(0) &= ~SIO_CTRL_DTR;

	return respLength;
}

#define MEMCARD_CMD_GET_ID 0x53
#define MEMCARD_CMD_READ    0x52
#define MEMCARD_CMD_WRITE   0x57

// Writes one 128-byte sector to a memory card. Protocol confirmed
// directly from psx-spx's "Writing data to Memory Card" table:
//   Send 57h ('W')       -> receive FLAG
//   Send 00h, 00h        -> receive ID1(5Ah), ID2(5Dh)
//   Send addrMSB,addrLSB -> receive (00h), (pre)
//   Send 128 data bytes  -> receive dummy values
//   Send checksum        -> receive dummy
//   Send 00h, 00h        -> receive ACK1(5Ch), ACK2(5Dh)
//   Send 00h             -> receive end byte
// 137 bytes total each way (1+2+2+128+1+2+1). Checksum = addrMSB XOR
// addrLSB XOR all 128 data bytes - same XOR convention psx-spx documents
// for both this and the directory-frame checksum field. Returns the end
// byte: 0x47=Good, 0x4E=BadChecksum, 0xFF=BadSector, or 0 if the card
// didn't respond to the initial exchange at all. psx-spx notes Write
// doesn't have Read's extra mid-command delay, so the short timeout is
// used here rather than the long one - but this has NOT yet been
// confirmed on real hardware, unlike Read Sector's timeout, which took
// real debugging to get right. Treat this as unverified until tested.
static uint8_t writeMemoryCardSector(
	int      port,
	uint16_t sector,
	const uint8_t data[128]
) {
	uint8_t request[137]  = { 0 };
	uint8_t response[137] = { 0 };

	uint8_t addrMSB = (uint8_t) (sector >> 8);
	uint8_t addrLSB = (uint8_t) (sector & 0xff);

	request[0] = MEMCARD_CMD_WRITE;
	request[3] = addrMSB;
	request[4] = addrLSB;

	uint8_t checksum = (uint8_t) (addrMSB ^ addrLSB);
	for (int i = 0; i < 128; i++) {
		request[5 + i] = data[i];
		checksum       = (uint8_t) (checksum ^ data[i]);
	}
	request[133] = checksum;

	selectControllerPort(port);
	size_t respLength = memcardExchangePacket(
		request, response, sizeof(request), sizeof(response),
		MEMCARD_TIMEOUT_SHORT
	);

	if (respLength < 137)
		return 0;

	return response[136]; // end byte
}

// Reads one 128-byte sector from a memory card. Protocol confirmed
// directly from psx-spx's "Reading data from Memory Card" table:
//   Send 52h ('R')      -> receive FLAG
//   Send 00h, 00h       -> receive ID1(5Ah), ID2(5Dh)
//   Send addrMSB,addrLSB-> receive (00h), (pre)
//   Send 00h, 00h       -> receive ACK1(5Ch), ACK2(5Dh)
//   Send 00h, 00h       -> receive confirmed address MSB, LSB
//   Send 00h x128       -> receive the 128 data bytes
//   Send 00h            -> receive checksum
//   Send 00h            -> receive end byte (47h = Good)
// 139 bytes total each way. Returns the end byte (0x47 on success),
// or 0 if the card didn't respond to the initial exchange at all.
static uint8_t readMemoryCardSector(
	int      port,
	uint16_t sector,
	uint8_t  data[128],
	size_t   *outRespLength,
	uint8_t  header[9]
) {
	uint8_t request[139]  = { 0 };
	uint8_t response[139] = { 0 };

	request[0] = MEMCARD_CMD_READ;
	request[3] = (uint8_t) (sector >> 8);   // address MSB
	request[4] = (uint8_t) (sector & 0xff); // address LSB

	selectControllerPort(port);
	size_t respLength = memcardExchangePacket(
		request, response, sizeof(request), sizeof(response),
		MEMCARD_TIMEOUT_READ
	);

	*outRespLength = respLength;
	for (int i = 0; i < 9; i++)
		header[i] = response[i];

	if (respLength < 139) {
		for (int i = 0; i < 128; i++)
			data[i] = 0;
		return 0;
	}

	for (int i = 0; i < 128; i++)
		data[i] = response[9 + i];

	return response[138]; // end byte
}

static bool detectMemoryCard(int port, uint8_t response[9]) {
	uint8_t request[9] = { MEMCARD_CMD_GET_ID, 0, 0, 0, 0, 0, 0, 0, 0 };

	selectControllerPort(port);
	size_t respLength = memcardExchangePacket(
		request, response, sizeof(request), 9,
		MEMCARD_TIMEOUT_SHORT
	);

	if (respLength < 3)
		return false;

	// ID1/ID2 are the reliable presence indicator - the full 04,00,00,80
	// tail is documented as Sony-card-specific and may not be present on
	// third-party cards.
	return (response[1] == 0x5a) && (response[2] == 0x5d);
}
/* ---- Directory parsing ----
 *
 * Allocation state byte: 0x51 (first block of a used file) and 0xA1
 * (first block of a deleted file) confirmed directly from psx-spx's
 * Memory Card Data Format page ("State=51h or A1h" is where
 * filename/filesize are actually stored). 0xA0 (free) is standard,
 * universally-documented PS1 convention - every memory card tool agrees
 * on this specific value, it isn't unique to one source. Middle/end-link
 * states for multi-block saves (0x52/0x53 used, 0xA2/0xA3 deleted)
 * follow that same well-established convention but weren't individually
 * re-confirmed this round - flagged here for anyone revisiting this.
 *
 * Filename location (offset 0x0A, up to 20 bytes) also confirmed
 * directly from psx-spx.
 */

#define MC_STATE_FREE           0xa0
#define MC_STATE_USED_FIRST     0x51
#define MC_STATE_USED_MIDDLE    0x52
#define MC_STATE_USED_LAST      0x53
#define MC_STATE_DELETED_FIRST  0xa1
#define MC_STATE_DELETED_MIDDLE 0xa2
#define MC_STATE_DELETED_LAST   0xa3

typedef enum {
	DELETE_OK,
	DELETE_FAILED
} DeleteResult;

typedef enum {
	COPY_OK,
	COPY_FAILED,
	COPY_NO_DESTINATION,
	COPY_NO_FREE_BLOCK,
	COPY_DUPLICATE
} CopyResult;


typedef enum {
	FORMAT_OK,
	FORMAT_FAILED
} FormatResult;

// Formats the whole card: writes "MC" + zeros to the card's own header
// (sector 0, matching psx-spx's documented format for that sector), and
// 0xA0 ("free, freshly formatted") + zeros to all 15 directory frames
// (sectors 1-15), including resetting the next-block pointer to FFFFh
// (no chain) on each. This is the highest-risk write operation here -
// unlike Delete, which only ever touches the blocks belonging to one
// save, this touches every block's directory entry on the card at once.
static FormatResult formatCard(int port) {
	uint8_t sectorData[128];

	for (int i = 0; i < 128; i++)
		sectorData[i] = 0;
	sectorData[0] = 'M';
	sectorData[1] = 'C';
	sectorData[127] = (uint8_t) ('M' ^ 'C');

	if (writeMemoryCardSector(port, 0, sectorData) != 0x47)
		return FORMAT_FAILED;

	for (int block = 1; block <= 15; block++) {
		for (int i = 0; i < 128; i++)
			sectorData[i] = 0;
		sectorData[0]    = MC_STATE_FREE;
		sectorData[0x08] = 0xff;
		sectorData[0x09] = 0xff;

		uint8_t checksum = 0;
		for (int i = 0; i < 127; i++)
			checksum = (uint8_t) (checksum ^ sectorData[i]);
		sectorData[127] = checksum;

		if (writeMemoryCardSector(port, block, sectorData) != 0x47)
			return FORMAT_FAILED;
	}

	return FORMAT_OK;
}

// Shared logic for both Delete and Undelete: follows the block chain,
// collecting every linked block (read-only, so a read failure changes
// nothing), then flips each block's state in the requested direction
// and writes it back in REVERSE order (last block first) - so a write
// failure partway through leaves the first block (which determines
// whether the directory considers the save "there" at all) as the LAST
// thing touched, keeping a partial failure looking consistent rather
// than corrupted either way.
static DeleteResult setSaveState(int port, int block, bool toDeleted) {
	int chain[15];
	int chainLen = 0;
	int currentBlock = block;

	for (int step = 0; step < 15; step++) {
		uint8_t sectorData[128];
		size_t  respLength;
		uint8_t header[9];

		uint8_t endByte = readMemoryCardSector(
			port, currentBlock, sectorData, &respLength, header
		);
		if (endByte != 0x47)
			return DELETE_FAILED;

		uint8_t state = sectorData[0];
		bool isFirst, isMiddle, isLast;
		if (toDeleted) {
			isFirst  = (state == MC_STATE_USED_FIRST);
			isMiddle = (state == MC_STATE_USED_MIDDLE);
			isLast   = (state == MC_STATE_USED_LAST);
		} else {
			isFirst  = (state == MC_STATE_DELETED_FIRST);
			isMiddle = (state == MC_STATE_DELETED_MIDDLE);
			isLast   = (state == MC_STATE_DELETED_LAST);
		}
		if (!isFirst && !isMiddle && !isLast)
			return DELETE_FAILED;
		if ((step == 0) && !isFirst)
			return DELETE_FAILED; // must start on the save's actual first block

		chain[chainLen++] = currentBlock;

		uint16_t nextPtr = (uint16_t) (sectorData[0x08] | (sectorData[0x09] << 8));
		if (nextPtr == 0xffff)
			break;
		currentBlock = (int) nextPtr + 1;
	}

	for (int i = chainLen - 1; i >= 0; i--) {
		uint8_t sectorData[128];
		size_t  respLength;
		uint8_t header[9];

		uint8_t endByte = readMemoryCardSector(
			port, chain[i], sectorData, &respLength, header
		);
		if (endByte != 0x47)
			return DELETE_FAILED;

		uint8_t state = sectorData[0];
		if (toDeleted) {
			if (state == MC_STATE_USED_FIRST)
				sectorData[0] = MC_STATE_DELETED_FIRST;
			else if (state == MC_STATE_USED_MIDDLE)
				sectorData[0] = MC_STATE_DELETED_MIDDLE;
			else if (state == MC_STATE_USED_LAST)
				sectorData[0] = MC_STATE_DELETED_LAST;
			else
				return DELETE_FAILED;
		} else {
			if (state == MC_STATE_DELETED_FIRST)
				sectorData[0] = MC_STATE_USED_FIRST;
			else if (state == MC_STATE_DELETED_MIDDLE)
				sectorData[0] = MC_STATE_USED_MIDDLE;
			else if (state == MC_STATE_DELETED_LAST)
				sectorData[0] = MC_STATE_USED_LAST;
			else
				return DELETE_FAILED;
		}

		uint8_t checksum = 0;
		for (int j = 0; j < 127; j++)
			checksum = (uint8_t) (checksum ^ sectorData[j]);
		sectorData[127] = checksum;

		uint8_t writeEndByte = writeMemoryCardSector(port, chain[i], sectorData);
		if (writeEndByte != 0x47)
			return DELETE_FAILED;
	}

	return DELETE_OK;
}

static DeleteResult deleteSave(int port, int block) {
	return setSaveState(port, block, true);
}

static DeleteResult undeleteSave(int port, int block) {
	return setSaveState(port, block, false);
}

// Follows a DELETED chain (same collection logic as setSaveState, just
// starting from the deleted states) and fully clears each block's
// directory frame - 0xA0 ("free, freshly formatted") plus zeroing
// everything else, matching Format's own per-block reset - rather than
// just flipping the state byte the way Undelete does. This makes the
// block genuinely, immediately free (usable as a copy destination
// right away) instead of merely "deleted" (recoverable, but still
// occupying its slot until formatted or cleared).
static DeleteResult clearChain(int port, int block) {
	int chain[15];
	int chainLen = 0;
	int currentBlock = block;

	for (int step = 0; step < 15; step++) {
		uint8_t sectorData[128];
		size_t  respLength;
		uint8_t header[9];
		uint8_t endByte = readMemoryCardSector(
			port, currentBlock, sectorData, &respLength, header
		);
		if (endByte != 0x47)
			return DELETE_FAILED;

		uint8_t state = sectorData[0];
		bool isFirst  = (state == MC_STATE_DELETED_FIRST);
		bool isMiddle = (state == MC_STATE_DELETED_MIDDLE);
		bool isLast   = (state == MC_STATE_DELETED_LAST);
		if (!isFirst && !isMiddle && !isLast)
			return DELETE_FAILED;
		if ((step == 0) && !isFirst)
			return DELETE_FAILED;

		chain[chainLen++] = currentBlock;

		uint16_t nextPtr = (uint16_t) (sectorData[0x08] | (sectorData[0x09] << 8));
		if (nextPtr == 0xffff)
			break;
		currentBlock = (int) nextPtr + 1;
	}

	for (int i = chainLen - 1; i >= 0; i--) {
		uint8_t sectorData[128];
		for (int j = 0; j < 128; j++)
			sectorData[j] = 0;
		sectorData[0]    = MC_STATE_FREE;
		sectorData[0x08] = 0xff;
		sectorData[0x09] = 0xff;

		uint8_t checksum = 0;
		for (int j = 0; j < 127; j++)
			checksum = (uint8_t) (checksum ^ sectorData[j]);
		sectorData[127] = checksum;

		if (writeMemoryCardSector(port, chain[i], sectorData) != 0x47)
			return DELETE_FAILED;
	}

	return DELETE_OK;
}

typedef struct {
	bool     read;    // did we successfully read this block's directory frame at all
	uint8_t  state;
	bool     used;
	bool     deleted;
	char     title[21];
	char     gameId[16];
	char     titleId[21];
	uint16_t nextBlockPtr;     // raw pointer (block-1) from offset 0x08-0x09, or 0xFFFF
	int      firstBlockOfChain; // 0 = n/a (free); else block number (1-15) of this save's actual first block - equals the block's own number for a first block itself, or the first block's number for a continuation block
} DirectoryEntry;

// Copies a single-block save to the first free block on the OTHER slot.
// Deliberately does not support multi-block saves yet, matching the
// same conservative approach the first Delete implementation took -
// copying a whole chain correctly means finding multiple free
// destination blocks and adjusting every block's own next-block
// pointer to match its new position, which deserves its own careful
// verification separately. A save's own data (title/icon/save frames)
// lives at global sectors [block*64, block*64+64) - a completely
// different address range from its directory frame (which lives at
// global sector = block number, within block 0) - both need copying.
static CopyResult copySave(
	int srcPort,
	int srcBlock,
	const DirectoryEntry destEntries[15]
) {
	int dstPort = 1 - srcPort;

	// Collect the full source chain first (read-only).
	int srcChain[15];
	int chainLen = 0;
	int currentBlock = srcBlock;

	for (int step = 0; step < 15; step++) {
		uint8_t sectorData[128];
		size_t  respLength;
		uint8_t header[9];
		uint8_t endByte = readMemoryCardSector(
			srcPort, currentBlock, sectorData, &respLength, header
		);
		if (endByte != 0x47)
			return COPY_FAILED;

		uint8_t state = sectorData[0];
		bool isFirst  = (state == MC_STATE_USED_FIRST);
		bool isMiddle = (state == MC_STATE_USED_MIDDLE);
		bool isLast   = (state == MC_STATE_USED_LAST);
		if (!isFirst && !isMiddle && !isLast)
			return COPY_FAILED;
		if ((step == 0) && !isFirst)
			return COPY_FAILED;

		srcChain[chainLen++] = currentBlock;

		uint16_t nextPtr = (uint16_t) (sectorData[0x08] | (sectorData[0x09] << 8));
		if (nextPtr == 0xffff)
			break;
		currentBlock = (int) nextPtr + 1;
	}

	// Find enough usable destination blocks, one per chain link. Genuinely
	// free (0xA0) AND deleted blocks both count - a deleted block gets
	// implicitly cleared by the copy itself, since the copy fully
	// overwrites the destination's directory frame (including its state
	// byte) with the source's own data.
	int dstChain[15];
	int foundCount = 0;
	for (int i = 0; (i < 15) && (foundCount < chainLen); i++) {
		if (!destEntries[i].used)
			dstChain[foundCount++] = i + 1;
	}
	if (foundCount < chainLen)
		return COPY_NO_FREE_BLOCK;

	// Copy each block: directory frame (with its next-block pointer
	// rewritten to the NEW destination block, since destination free
	// blocks can land at different numeric positions than the source
	// chain) plus all 64 of the block's own data sectors.
	for (int i = 0; i < chainLen; i++) {
		uint8_t dirSector[128];
		size_t  respLength;
		uint8_t header[9];
		uint8_t endByte = readMemoryCardSector(
			srcPort, srcChain[i], dirSector, &respLength, header
		);
		if (endByte != 0x47)
			return COPY_FAILED;

		if (i + 1 < chainLen) {
			uint16_t newNextPtr = (uint16_t) (dstChain[i + 1] - 1);
			dirSector[0x08] = (uint8_t) (newNextPtr & 0xff);
			dirSector[0x09] = (uint8_t) (newNextPtr >> 8);
		} else {
			dirSector[0x08] = 0xff;
			dirSector[0x09] = 0xff;
		}

		uint8_t checksum = 0;
		for (int j = 0; j < 127; j++)
			checksum = (uint8_t) (checksum ^ dirSector[j]);
		dirSector[127] = checksum;

		if (writeMemoryCardSector(dstPort, dstChain[i], dirSector) != 0x47)
			return COPY_FAILED;

		for (int frame = 0; frame < 64; frame++) {
			uint8_t sectorData[128];
			uint8_t rEnd = readMemoryCardSector(
				srcPort, srcChain[i] * 64 + frame, sectorData, &respLength, header
			);
			if (rEnd != 0x47)
				return COPY_FAILED;

			uint8_t wEnd = writeMemoryCardSector(
				dstPort, dstChain[i] * 64 + frame, sectorData
			);
			if (wEnd != 0x47)
				return COPY_FAILED;
		}
	}

	return COPY_OK;
}

// Searches the raw title for a known region-code pattern
// (SLUS/SLES/SLPS/SCUS/SCES/SCPS + hyphen + up to 5 digits) anywhere
// within it, not just at the start - this correctly handles
// publisher-added prefixes like "BA" or "BI" (eg. "BASLUS-00402TEKKEN-3"
// -> gameId "SLUS-00402", titleId "TEKKEN-3"), verified directly against
// both examples given. Not a fixed-offset field from psx-spx - this is a
// parse built on top of the naming convention psx-spx does document
// (region code + hyphen + numeric ID), not a guaranteed-universal one.
static const char *const REGION_CODES[] = {
	"SLUS", "SLES", "SLPS", "SCUS", "SCES", "SCPS"
};
#define NUM_REGION_CODES 6

static void parseGameIdAndTitle(
	const char *rawTitle,
	char       *gameId,
	size_t     gameIdSize,
	char       *titleId,
	size_t     titleIdSize
) {
	gameId[0]  = '\0';
	titleId[0] = '\0';

	const char *found = NULL;
	for (int i = 0; i < NUM_REGION_CODES; i++) {
		const char *code = REGION_CODES[i];
		for (const char *p = rawTitle; *p; p++) {
			bool match = true;
			for (int j = 0; j < 4; j++) {
				if (!p[j] || (p[j] != code[j])) {
					match = false;
					break;
				}
			}
			if (match && (!found || p < found)) {
				found = p;
				break;
			}
		}
	}

	// No recognizable region code, or not followed by "-digits" - just
	// pass the raw title through as the title ID.
	if (!found || (found[4] != '-')) {
		int i;
		for (i = 0; ((size_t) i < titleIdSize - 1) && rawTitle[i]; i++)
			titleId[i] = rawTitle[i];
		titleId[i] = '\0';
		return;
	}

	int len    = 5; // 4-letter code + hyphen
	int digits = 0;
	while (found[len] && (digits < 5) && (found[len] >= '0') && (found[len] <= '9')) {
		len++;
		digits++;
	}

	if ((digits > 0) && ((size_t) len < gameIdSize)) {
		for (int i = 0; i < len; i++)
			gameId[i] = found[i];
		gameId[len] = '\0';
	}

	const char *titleStart = found + len;
	int i;
	for (i = 0; ((size_t) i < titleIdSize - 1) && titleStart[i]; i++)
		titleId[i] = titleStart[i];
	titleId[i] = '\0';
}

static void parseDirectoryFrame(const uint8_t frame[128], DirectoryEntry *entry) {
	entry->state = frame[0];
	entry->used  = (entry->state == MC_STATE_USED_FIRST)
	            || (entry->state == MC_STATE_USED_MIDDLE)
	            || (entry->state == MC_STATE_USED_LAST);
	entry->deleted = (entry->state == MC_STATE_DELETED_FIRST)
	               || (entry->state == MC_STATE_DELETED_MIDDLE)
	               || (entry->state == MC_STATE_DELETED_LAST);
	entry->nextBlockPtr = (uint16_t) (frame[0x08] | (frame[0x09] << 8));
	entry->firstBlockOfChain = 0; // computed in a second pass, after all 15 blocks are read

	entry->title[0]   = '\0';
	entry->gameId[0]  = '\0';
	entry->titleId[0] = '\0';
	if (entry->used || entry->deleted) {
		int i;
		for (i = 0; i < 20; i++) {
			uint8_t c = frame[0x0a + i];
			if (!c)
				break;
			entry->title[i] = ((c >= 0x20) && (c < 0x7f)) ? (char) c : '.';
		}
		entry->title[i] = '\0';
		parseGameIdAndTitle(
			entry->title,
			entry->gameId, sizeof(entry->gameId),
			entry->titleId, sizeof(entry->titleId)
		);
	}
}

/* ---- Icon decoding ----
 *
 * Confirmed directly from psx-spx's Memory Card Data Format page. The
 * icon is NOT part of the directory frame we already read (block 0) -
 * it lives in the save's OWN first data block, split across two
 * sectors:
 *   Sector (block*64 + 0): "Title Frame" - ID "SC", icon display flag,
 *     Shift-JIS title, and the 16-color CLUT at offset 0x60 (16 entries,
 *     2 bytes each, standard PS1 5-5-5-1 BGR format - same format used
 *     throughout the rest of this project's texture/palette code).
 *   Sector (block*64 + 1): first icon bitmap frame - 16x16 pixels,
 *     4-bit indexed (2 pixels per byte, low nibble first), exactly 128
 *     bytes = one full sector. Up to 2 more animation frames follow in
 *     the next sectors if the display flag says so, not read here yet.
 *
 * Rendered as a small downsampled (8x8) preview using drawRect per
 * pixel block, NOT a full 16x16 pixel-by-pixel render or a real GPU
 * texture upload - 256 individual rects for a full-resolution render
 * would cost roughly 1280 words on its own, and this project has
 * already hit the GPU packet budget twice building other screens. A
 * real texture-based render (matching how the actual BIOS draws icons,
 * via a textured polygon) is a reasonable follow-up once VRAM placement
 * for it has been planned out carefully.
 */

typedef struct {
	bool     valid;
	int      numFrames; // 1-3, from the Icon Display Flag (0x02: 11h/12h/13h)
	uint16_t clut[16];
	uint8_t  bitmap[3][128];
} IconData;

static bool readIcon(int port, int block, IconData *icon) {
	uint8_t titleFrame[128];
	size_t  respLength;
	uint8_t header[9];

	int baseSector = block * 64;

	uint8_t endByte0 = readMemoryCardSector(
		port, baseSector, titleFrame, &respLength, header
	);

	if ((endByte0 != 0x47) || (titleFrame[0] != 'S') || (titleFrame[1] != 'C')) {
		icon->valid = false;
		return false;
	}

	// Icon Display Flag: 11h=1 frame (static), 12h=2 frames, 13h=3
	// frames, both animated ones cycling automatically on real hardware
	// (16 PAL frames per step for 2-frame, 11 for 3-frame - psx-spx).
	uint8_t flag = titleFrame[0x02];
	int     numFrames;
	switch (flag) {
		case 0x12: numFrames = 2; break;
		case 0x13: numFrames = 3; break;
		default:   numFrames = 1; break;
	}

	for (int i = 0; i < 16; i++) {
		icon->clut[i] = (uint16_t) (
			titleFrame[0x60 + i * 2] | (titleFrame[0x60 + i * 2 + 1] << 8)
		);
	}

	for (int f = 0; f < numFrames; f++) {
		uint8_t bitmapFrame[128];
		uint8_t endByte = readMemoryCardSector(
			port, baseSector + 1 + f, bitmapFrame, &respLength, header
		);
		if (endByte != 0x47) {
			// Couldn't read this frame - fall back to whatever we did
			// get successfully rather than failing the whole icon.
			numFrames = f;
			break;
		}
		for (int i = 0; i < 128; i++)
			icon->bitmap[f][i] = bitmapFrame[i];
	}

	if (numFrames < 1) {
		icon->valid = false;
		return false;
	}

	icon->numFrames = numFrames;
	icon->valid     = true;
	return true;
}

// Grid icons: up to 30 of them (2 cards x 15 blocks), each needs its
// own dedicated, non-overlapping VRAM slot since they're all uploaded
// once and displayed simultaneously. Laid out as a systematic 12-per-
// row grid in VRAM, 32 halfwords apart horizontally (image + palette)
// and 17 lines apart vertically (16 for the icon + 1 for its palette
// row), starting well below the background/font textures and their
// palettes (which end around Y=112).
#define MC_ICON_VRAM_COLS   12
#define MC_ICON_VRAM_ROW_H  17
#define MC_ICON_VRAM_BASE_Y 130

static void uploadGridIconTexture(
	RenderContext *ctx,
	const IconData *icon,
	int index,
	TextureInfo *texInfo
) {
	int textureX = ctx->screenWidth * 2;

	int col = index % MC_ICON_VRAM_COLS;
	int row = index / MC_ICON_VRAM_COLS;

	int slotX = textureX + col * 32;
	int slotY = MC_ICON_VRAM_BASE_Y + row * MC_ICON_VRAM_ROW_H;

	uploadIndexedTexture(
		texInfo,
		icon->bitmap[0],
		icon->clut,
		slotX, slotY,
		slotX + 16, slotY,
		16, 16,
		GP0_COLOR_4BPP
	);
}

// Uploads the icon's 16x16 4-bit bitmap and 16-color CLUT to a small
// unused VRAM region (safely below the background/font textures and
// their palettes, which occupy Y=0-112 at this same X - see
// reloadTextures() in renderer.c) and returns a TextureInfo describing
// where it landed, ready to be drawn as a real textured rect. This is
// the actual way the BIOS itself draws icons (psx-spx: "drawn via
// GP0(2Ch), as Textured four-point polygon") - full pixel-perfect
// quality, and roughly 5 words of GPU packet cost to draw versus the
// ~320 words the earlier downsampled rect-grid approach cost.
static void drawIconTexture(
	RenderContext *ctx,
	const TextureInfo *texInfo,
	int x,
	int y,
	int displaySize
) {
	GPUDMAChain *chain = getCurrentChain(ctx);
	uint32_t    *ptr;

	ptr    = allocateGP0Packet(chain, 1);
	ptr[0] = gp0_setPage(texInfo->page, false, false);

	ptr    = allocateGP0Packet(chain, 4);
	ptr[0] = gp0_rectangle(true, true, false);
	ptr[1] = gp0_xy(x, y);
	ptr[2] = gp0_uv(texInfo->u, texInfo->v, texInfo->clut);
	ptr[3] = gp0_xy(displaySize, displaySize);
}


/* ---- Manager UI ---- */

#define MC_GRID_COLS 3
#define MC_GRID_ROWS 5

#define MC_CELL_W 24
#define MC_CELL_H 24

/*
 * Layout.
 *
 * The two 3-wide grids are centred as a pair rather than pinned to the left,
 * and the whole screen starts lower down: the title used to sit at y=8, which
 * a CRT's overscan clips on most sets. 16px of margin on each edge is the
 * usual safe area for 320x240.
 *
 *   grid pair width = 2 * (3 * 24) + 40 gap = 184
 *   left margin     = (320 - 184) / 2       = 68
 */
#define MC_GRID_W     (MC_GRID_COLS * MC_CELL_W)   /* 72  */
#define MC_SLOT_GAP   40
#define MC_SLOT0_X    ((320 - (2 * MC_GRID_W + MC_SLOT_GAP)) / 2)
#define MC_SLOT1_X    (MC_SLOT0_X + MC_GRID_W + MC_SLOT_GAP)
#define MC_GRID_Y     40
#define MC_TITLE_Y    16

/*
 * The tile tints follow the current theme's dominant colour, so the grid
 * matches the wallpaper instead of being permanently blue - Nebula 3 gives
 * orange crystal, the Cosmos themes violet, and so on. Fetched once per frame
 * in drawMemoryCardManager() rather than per cell.
 */
static uint32_t mcAccent = 0x702810;   /* 0xBBGGRR, replaced each frame */
static uint32_t mcGlow   = 0xffd8a0;

/*
 * A readable panel: one flat tint, bevelled edge, no sheen.
 *
 * drawGlassPanel() is right for a small tile the eye skims over, but wrong
 * behind a block of text - its sheen puts two different tones behind the same
 * line. Here the body is drawn twice so it settles to about 75% opacity
 * (blending is a fixed 50% mix, so each pass halves what shows through), then
 * the same top-left/bottom-right bevel ties it to the tiles around it.
 */
static void drawGlassCard(
	RenderContext *ctx, int x, int y, int w, int h, uint32_t tint
);

/* Local shade helper; the tiles themselves are drawn by drawGlassPanel(). */
static uint32_t mcScale(uint32_t colour, int numerator, int denominator) {
	uint32_t r = ((colour        & 0xff) * numerator) / denominator;
	uint32_t g = (((colour >> 8)  & 0xff) * numerator) / denominator;
	uint32_t b = (((colour >> 16) & 0xff) * numerator) / denominator;

	if (r > 0xff) r = 0xff;
	if (g > 0xff) g = 0xff;
	if (b > 0xff) b = 0xff;

	return (b << 16) | (g << 8) | r;
}

static void drawGlassCard(
	RenderContext *ctx, int x, int y, int w, int h, uint32_t tint
) {
	uint32_t body = mcScale(tint, 3, 4);   /* a touch darker than the tiles */

	drawRect(ctx, x,     y + 1,     w,     h - 2, body, true);
	drawRect(ctx, x,     y + 1,     w,     h - 2, body, true);
	drawRect(ctx, x + 1, y,         w - 2, 1,     body, true);
	drawRect(ctx, x + 1, y + h - 1, w - 2, 1,     body, true);

	uint32_t lit   = mcScale(tint, 5, 2);
	uint32_t shade = mcScale(tint, 1, 3);

	drawRect(ctx, x + 1,     y,         w - 2, 1,     lit,   true);
	drawRect(ctx, x,         y + 1,     1,     h - 2, lit,   true);
	drawRect(ctx, x + 1,     y + h - 1, w - 2, 1,     shade, true);
	drawRect(ctx, x + w - 1, y + 1,     1,     h - 2, shade, true);
}


typedef enum {
	MENU_OPTION_COPY,
	MENU_OPTION_DELETE,
	MENU_OPTION_CLEAR,
	MENU_OPTION_FORMAT,
	MENU_OPTION_CANCEL,
	MENU_NUM_OPTIONS
} ContextMenuOption;

typedef enum {
	STAGE_BROWSING,
	STAGE_MENU,
	STAGE_CONFIRM
} UIStage;

// Loads (or reloads) one slot's full 15-block directory, including grid
// icon uploads for used blocks. Used both for the initial load and for
// refreshing a slot after a card swap is detected.
// A cell is selectable unless it's a continuation block of a
// multi-block save - the chain's own first block, and anything not
// part of a chain at all (free/deleted/standalone), is always
// selectable. Continuation blocks are skipped entirely during
// navigation so the whole chain behaves as one save.
static bool isSelectableCell(const DirectoryEntry entries[15], int index) {
	return (entries[index].firstBlockOfChain == 0)
	    || (entries[index].firstBlockOfChain == index + 1);
}

static void loadSlotDirectory(
	RenderContext  *ctx,
	int             slot,
	DirectoryEntry  entries[15],
	TextureInfo     gridIconTex[15],
	bool            gridIconValid[15]
) {
	for (int i = 0; i < 15; i++) {
		entries[i].read = false;
		gridIconValid[i] = false;
	}

	uint8_t sectorData[128];
	for (int block = 1; block <= 15; block++) {
		size_t  respLength;
		uint8_t header[9];
		uint8_t endByte = readMemoryCardSector(
			slot, block, sectorData, &respLength, header
		);

		DirectoryEntry *entry = &entries[block - 1];
		if (endByte == 0x47) {
			parseDirectoryFrame(sectorData, entry);
			entry->read = true;

			// Only a save's actual first block has its own icon -
			// continuation blocks never do, so don't waste reads
			// attempting one for them.
			if (entry->state == MC_STATE_USED_FIRST) {
				static IconData gridIcon;
				if (readIcon(slot, block, &gridIcon) && gridIcon.valid) {
					int index = slot * 15 + (block - 1);
					uploadGridIconTexture(
						ctx, &gridIcon, index, &gridIconTex[block - 1]
					);
					gridIconValid[block - 1] = true;
				}
			}
		} else {
			entry->read = false;
		}
	}

	// Second pass: for every save's first block, follow its chain and
	// propagate firstBlockOfChain, title/gameId/titleId, and the shared
	// grid icon reference to every continuation block - so the whole
	// chain visually reads as one save (same icon, same info) rather
	// than looking like separate, unrelated blocks.
	for (int i = 0; i < 15; i++) {
		DirectoryEntry *first = &entries[i];
		if (!first->read || (first->state != MC_STATE_USED_FIRST))
			continue;

		first->firstBlockOfChain = i + 1;

		uint16_t nextPtr = first->nextBlockPtr;
		int      steps   = 0;
		while ((nextPtr != 0xffff) && (steps < 15)) {
			int nextBlock = (int) nextPtr + 1;
			if ((nextBlock < 1) || (nextBlock > 15))
				break;

			DirectoryEntry *cont = &entries[nextBlock - 1];
			if (!cont->read)
				break;

			cont->firstBlockOfChain = i + 1;
			for (int c = 0; c < 21; c++) {
				cont->title[c]   = first->title[c];
				cont->titleId[c] = first->titleId[c];
			}
			for (int c = 0; c < 16; c++)
				cont->gameId[c] = first->gameId[c];

			if (gridIconValid[i]) {
				gridIconTex[nextBlock - 1] = gridIconTex[i];
				gridIconValid[nextBlock - 1] = true;
			}

			nextPtr = cont->nextBlockPtr;
			steps++;
		}
	}
}

void runMemoryCardManager(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
) {
	(void) state;
	(void) item;

	uint8_t resp0[9] = { 0 };
	uint8_t resp1[9] = { 0 };
	bool present[2];
	present[0] = detectMemoryCard(0, resp0);
	present[1] = detectMemoryCard(1, resp1);

	// static, not stack-allocated: entries+gridIconTex+gridIconValid
	// together total ~2.4KB, which on its own already exceeds this
	// project's entire 2048-byte stack (see crt0.s, STACK_SIZE) before
	// counting anything else on the stack at all. This was a genuine,
	// confirmed stack overflow - it was corrupting whatever memory sits
	// adjacent to the stack buffer in .sbss, which is exactly why an
	// unrelated screen (RAM Tester) could crash afterward: the
	// corruption isn't scoped to this function's own data.
	static DirectoryEntry entries[2][15];
	static TextureInfo    gridIconTex[2][15];
	static bool           gridIconValid[2][15];

	for (int slot = 0; slot < 2; slot++) {
		for (int i = 0; i < 15; i++) {
			entries[slot][i].read  = false;
			gridIconValid[slot][i] = false;
		}

		if (present[slot]) {
			// Show a loading frame before each slot's blocking read
			// sequence, so the display stays actively updated instead
			// of sitting frozen on whatever was on screen before for
			// however long loading takes - longer with both cards
			// present than just one, which is very likely the actual
			// cause of the reported flashing on real hardware (no
			// frame renders at all between opening this screen and the
			// first completed load), not the periodic refresh check.
			beginFrame(ctx);
			drawXMBBackground(ctx);
			printString(ctx, 16, MC_TITLE_Y, 0x808080, "MEMORY CARD MANAGER");
			char loadingLine[32];
			snprintf(loadingLine, sizeof(loadingLine), "Reading slot %d...", slot + 1);
			printString(ctx, 24, 100, 0xffffff, loadingLine);
			endFrame(ctx);

			loadSlotDirectory(
				ctx, slot, entries[slot], gridIconTex[slot], gridIconValid[slot]
			);
		}
	}

	int      activeSlot   = present[0] ? 0 : 1;
	int      selected     = 0;
	uint16_t lastButtons  = 0;
	UIStage  stage        = STAGE_BROWSING;
	int      menuSelected = MENU_OPTION_COPY;
	char     notice[48]   = { 0 };
	int      noticeTimer  = 0;
	bool     startRefreshCandidate = false;

	char line[64];

	while (pollController(0) | pollController(1))
		;

	for (;;) {
		uint16_t buttons  = pollController(0) | pollController(1);
		uint16_t pressed  = buttons & ~lastButtons;
		uint16_t released = lastButtons & ~buttons;
		lastButtons       = buttons;

		if ((stage == STAGE_BROWSING) && (buttons & PAD_BTN_START) && (buttons & PAD_BTN_SELECT)) {
			playCancelSound();
			break;
		}

		bool haveAnyCard = present[0] || present[1];

		if (haveAnyCard) {
			if (stage == STAGE_BROWSING) {
				if (pressed & PAD_BTN_RIGHT) {
					int newSel = selected;
					while (newSel < 14) {
						newSel++;
						if (isSelectableCell(entries[activeSlot], newSel)) {
							selected = newSel;
							break;
						}
					}
				}
				if (pressed & PAD_BTN_LEFT) {
					int newSel = selected;
					while (newSel > 0) {
						newSel--;
						if (isSelectableCell(entries[activeSlot], newSel)) {
							selected = newSel;
							break;
						}
					}
				}
				if (pressed & PAD_BTN_DOWN) {
					int newSel = selected;
					while (newSel + MC_GRID_COLS < 15) {
						newSel += MC_GRID_COLS;
						if (isSelectableCell(entries[activeSlot], newSel)) {
							selected = newSel;
							break;
						}
					}
				}
				if (pressed & PAD_BTN_UP) {
					int newSel = selected;
					while (newSel - MC_GRID_COLS >= 0) {
						newSel -= MC_GRID_COLS;
						if (isSelectableCell(entries[activeSlot], newSel)) {
							selected = newSel;
							break;
						}
					}
				}

				if (present[0] && present[1]) {
					int newSlot = activeSlot;
					if (pressed & PAD_BTN_L1)
						newSlot = 0;
					else if (pressed & PAD_BTN_R1)
						newSlot = 1;

					if (newSlot != activeSlot) {
						activeSlot = newSlot;
						if (!isSelectableCell(entries[activeSlot], selected)) {
							int fallback = selected;
							for (int k = 0; k < 15; k++) {
								if (isSelectableCell(entries[activeSlot], k)) {
									fallback = k;
									break;
								}
							}
							selected = fallback;
						}
					}
				}

				if (pressed & PAD_BTN_CROSS) {
					// Only offer the menu for a block that has something in
					// it. Every option except Format acts on a save, and
					// Format is reachable from any used block on the card, so
					// opening this on a free slot only ever led to a dead end.
					const DirectoryEntry *cell =
						&entries[activeSlot][selected];

					if (cell->read && (cell->used || cell->deleted)) {
						stage        = STAGE_MENU;
						menuSelected = MENU_OPTION_COPY;
					}
				}
			} else if (stage == STAGE_MENU) {
				bool showClean = entries[activeSlot][selected].deleted;

				if (pressed & PAD_BTN_DOWN) {
					menuSelected = (menuSelected + 1) % MENU_NUM_OPTIONS;
					if (!showClean && (menuSelected == MENU_OPTION_CLEAR))
						menuSelected = (menuSelected + 1) % MENU_NUM_OPTIONS;
				}
				if (pressed & PAD_BTN_UP) {
					menuSelected = (menuSelected + MENU_NUM_OPTIONS - 1) % MENU_NUM_OPTIONS;
					if (!showClean && (menuSelected == MENU_OPTION_CLEAR))
						menuSelected = (menuSelected + MENU_NUM_OPTIONS - 1) % MENU_NUM_OPTIONS;
				}
				if (pressed & PAD_BTN_CIRCLE)
					stage = STAGE_BROWSING;
				if (pressed & PAD_BTN_CROSS) {
					if (menuSelected == MENU_OPTION_CANCEL) {
						stage = STAGE_BROWSING;
					} else if (menuSelected == MENU_OPTION_DELETE || menuSelected == MENU_OPTION_FORMAT || menuSelected == MENU_OPTION_CLEAR) {
						stage = STAGE_CONFIRM;
					} else {
						// Copy - non-destructive to both the source and
						// any existing destination data (only ever
						// writes to a confirmed-free block), so no
						// confirmation dialog needed.
						stage = STAGE_BROWSING;

						const DirectoryEntry *srcEntry = &entries[activeSlot][selected];

						if (!present[0] || !present[1]) {
							snprintf(notice, sizeof(notice), "Need a card in both slots");
						} else if (!srcEntry->used) {
							// Deleted/free blocks aren't real saves to
							// copy - deleted ones need Undelete first if
							// that's actually what's intended.
							snprintf(notice, sizeof(notice), "Nothing to copy here");
						} else {
							int dstSlot = 1 - activeSlot;
							bool alreadyExists = false;
							if (srcEntry->gameId[0]) {
								for (int i = 0; i < 15; i++) {
									if (entries[dstSlot][i].used
										&& (strcmp(entries[dstSlot][i].gameId, srcEntry->gameId) == 0)) {
										alreadyExists = true;
										break;
									}
								}
							}

							if (alreadyExists) {
								snprintf(notice, sizeof(notice), "Already exists on other card");
							} else {
								CopyResult result = copySave(
									activeSlot, selected + 1, entries[dstSlot]
								);

								if (result == COPY_OK) {
									loadSlotDirectory(
										ctx, dstSlot, entries[dstSlot],
										gridIconTex[dstSlot], gridIconValid[dstSlot]
									);
									snprintf(notice, sizeof(notice), "Copied to slot %d", dstSlot + 1);
								} else if (result == COPY_NO_FREE_BLOCK) {
									snprintf(notice, sizeof(notice), "Not enough free blocks");
								} else {
									snprintf(notice, sizeof(notice), "Copy failed");
								}
							}
						}
						noticeTimer = 120;
					}
				}
			} else if (stage == STAGE_CONFIRM) {
				if (pressed & PAD_BTN_CIRCLE)
					stage = STAGE_BROWSING;
				if (pressed & PAD_BTN_CROSS) {
					stage = STAGE_BROWSING;

					if (menuSelected == MENU_OPTION_DELETE) {
						bool wasDeleted = entries[activeSlot][selected].deleted;
						DeleteResult result = wasDeleted
							? undeleteSave(activeSlot, selected + 1)
							: deleteSave(activeSlot, selected + 1);

						if (result == DELETE_OK) {
							// Multi-block delete/undelete can affect
							// several grid cells at once, not just the
							// one selected - reload the whole slot
							// rather than trying to patch just one entry.
							loadSlotDirectory(
								ctx, activeSlot, entries[activeSlot],
								gridIconTex[activeSlot], gridIconValid[activeSlot]
							);
							snprintf(notice, sizeof(notice), wasDeleted ? "Undeleted" : "Deleted");
						} else {
							snprintf(notice, sizeof(notice), wasDeleted ? "Undelete failed" : "Delete failed");
						}
					} else if (menuSelected == MENU_OPTION_CLEAR) {
						if (!entries[activeSlot][selected].deleted) {
							snprintf(notice, sizeof(notice), "Nothing to clear here");
						} else {
							DeleteResult result = clearChain(activeSlot, selected + 1);

							if (result == DELETE_OK) {
								loadSlotDirectory(
									ctx, activeSlot, entries[activeSlot],
									gridIconTex[activeSlot], gridIconValid[activeSlot]
								);
								snprintf(notice, sizeof(notice), "Block cleared");
							} else {
								snprintf(notice, sizeof(notice), "Clear failed");
							}
						}
					} else if (menuSelected == MENU_OPTION_FORMAT) {
						FormatResult result = formatCard(activeSlot);

						if (result == FORMAT_OK) {
							loadSlotDirectory(
								ctx, activeSlot, entries[activeSlot],
								gridIconTex[activeSlot], gridIconValid[activeSlot]
							);
							selected      = 0;
							snprintf(notice, sizeof(notice), "Card formatted");
						} else {
							snprintf(notice, sizeof(notice), "Format failed");
						}
					}
					noticeTimer = 120;
				}
			}
		}

		if (noticeTimer > 0)
			noticeTimer--;

		// Manual refresh via START alone (not combined with SELECT,
		// which is reserved for exiting) - auto-refresh was removed
		// entirely after repeated rounds of flickering that kept
		// tracing back to the same root issue: detectMemoryCard() is a
		// real, blocking SIO0 exchange, and doing it periodically in
		// the background kept causing frame hitches no matter how
		// infrequently or carefully it was scheduled. A manual refresh
		// is simpler and fully predictable - it only costs a stall when
		// you actually ask for one, right after swapping a card.
		//
		// A previous version of this debounced by requiring START held
		// alone for a fixed ~15-frame window before firing. That was
		// still a race: if SELECT joined even one frame *after* that
		// window closed (entirely plausible - human fingers aren't
		// perfectly synced), refresh fired first, and only the *next*
		// START+SELECT press actually exited. Symptom on real hardware:
		// "exiting resets the memory card manager, then exiting again
		// actually exits."
		//
		// Fixed properly by removing the timing race instead of
		// shrinking it: track whether SELECT ever joined *at any point*
		// during the current START hold. Refresh only fires on START's
		// RELEASE, and only if SELECT never joined that hold. If SELECT
		// does join while START is still down, the exit check above
		// fires immediately on that same frame (it's checked first,
		// every frame) and breaks out before this code ever runs again
		// - so there's no window where a slow-but-genuine combo press
		// can be misread as a refresh request.
		if (stage == STAGE_BROWSING) {
			if (pressed & PAD_BTN_START)
				startRefreshCandidate = true;
			if ((buttons & PAD_BTN_START) && (buttons & PAD_BTN_SELECT))
				startRefreshCandidate = false;
		} else {
			startRefreshCandidate = false;
		}

		if ((stage == STAGE_BROWSING) && (released & PAD_BTN_START) && startRefreshCandidate) {
			startRefreshCandidate = false;
			present[0] = detectMemoryCard(0, resp0);
			present[1] = detectMemoryCard(1, resp1);

			for (int slot = 0; slot < 2; slot++) {
				if (present[slot]) {
					loadSlotDirectory(
						ctx, slot, entries[slot],
						gridIconTex[slot], gridIconValid[slot]
					);
				} else {
					for (int i = 0; i < 15; i++) {
						entries[slot][i].read  = false;
						gridIconValid[slot][i] = false;
					}
				}
			}

			if (!present[activeSlot] && (present[0] || present[1]))
				activeSlot = present[0] ? 0 : 1;
			selected = 0;
		}

		beginFrame(ctx);
		drawXMBBackground(ctx);

		// Follow the wallpaper: picked up every frame so switching theme in
		// Settings and coming back here recolours the grid immediately.
		xmbGetAccentColor(&mcAccent, &mcGlow);

		printString(ctx, 16, MC_TITLE_Y, 0x808080, "MEMORY CARD MANAGER");

		if (!haveAnyCard) {
			printString(ctx, 24, 40, 0x808080, "No card detected in either slot");
		} else {
			for (int slot = 0; slot < 2; slot++) {
				int baseX = (slot == 0) ? MC_SLOT0_X : MC_SLOT1_X;

				snprintf(line, sizeof(line), "Slot %d", slot + 1);
				printString(ctx, baseX, MC_GRID_Y - 12, 0x505050, line);

				if (!present[slot]) {
					printString(ctx, baseX, MC_GRID_Y, 0x808080, "(empty)");
					printString(ctx, baseX, MC_GRID_Y + 14, 0x505050, "START");
					printString(ctx, baseX, MC_GRID_Y + 26, 0x505050, "to scan");
					continue;
				}

				for (int i = 0; i < 15; i++) {
					int col = i % MC_GRID_COLS;
					int row = i / MC_GRID_COLS;
					int x   = baseX + col * MC_CELL_W;
					int y   = MC_GRID_Y + row * MC_CELL_H;

					const DirectoryEntry *entry = &entries[slot][i];

					// Tints are deliberately dark: drawGlassPanel() lightens
					// them for the sheen and the bevel, and blending lets the
					// wallpaper through, so a mid-brightness tint here comes
					// out washed rather than glassy. 0xBBGGRR.
					// Occupied and free blocks are the theme accent at two
					// brightnesses, so the grid reads as one material. Deleted
					// keeps a fixed amber and unreadable a neutral grey: those
					// two carry meaning, and tinting them would make them
					// indistinguishable from "used" under a warm theme.
					uint32_t color;
					if (!entry->read)
						color = 0x1a1a1a;                  // unreadable
					else if (entry->used)
						color = mcAccent;                  // occupied
					else if (entry->deleted)
						color = 0x184048;                  // deleted: amber
					else
						color = mcScale(mcAccent, 2, 5);   // free: dimmer

					bool isSelected = (slot == activeSlot) && (i == selected);
					bool willShowIcon = entry->used && gridIconValid[slot][i];

					drawGlassPanel(
						ctx, x, y, MC_CELL_W - 3, MC_CELL_H - 3,
						color, isSelected ? mcGlow : 0
					);

					if (willShowIcon) {
						// The icon sits on top of the glass rather than
						// replacing it, so the bevel still frames it.
						drawIconTexture(
							ctx, &gridIconTex[slot][i],
							x + 3, y + 3, MC_CELL_W - 9
						);
					} else {
						snprintf(line, sizeof(line), "%02d", i + 1);
						printString(ctx, x + 3, y + 3, 0xffffff, line);
					}
				}
			}

			// +4 rather than +8: the panel's four lines have to finish above the
			// control hints at y=200, and the grid now starts lower down to
			// keep the title out of overscan.
			int panelY = MC_GRID_Y + MC_GRID_ROWS * MC_CELL_H + 4;

			const DirectoryEntry *sel = present[activeSlot] ? &entries[activeSlot][selected] : NULL;

			if (sel) {
				if (!sel->read) {
					printString(ctx, 16, panelY, 0x808080, "Could not read this block");
				} else if (sel->used) {
					printString(ctx, 16, panelY, 0xffffff, "Status: USED");
					snprintf(line, sizeof(line), "Title ID : %s", sel->titleId[0] ? sel->titleId : sel->title);
					printString(ctx, 16, panelY + 12, 0xffffff, line);
					snprintf(line, sizeof(line), "Game ID : %s", sel->gameId[0] ? sel->gameId : "?");
					printString(ctx, 16, panelY + 24, 0xffffff, line);
				} else if (sel->deleted) {
					printString(ctx, 16, panelY, 0xffffff, "Status: DELETED");
					snprintf(line, sizeof(line), "Title ID : %s", sel->titleId[0] ? sel->titleId : sel->title);
					printString(ctx, 16, panelY + 12, 0xffffff, line);
					snprintf(line, sizeof(line), "Game ID : %s", sel->gameId[0] ? sel->gameId : "?");
					printString(ctx, 16, panelY + 24, 0xffffff, line);
				} else {
					printString(ctx, 16, panelY, 0x808080, "Status: FREE");
				}
			}

			if (noticeTimer > 0)
				printString(ctx, 16, panelY + 30, 0x1256e3, notice);

			if (stage == STAGE_MENU) {
				bool showClean = sel && sel->deleted;

				const char *titleLine =
					"Options  " CH_PS1_CROSS_BUTTON ": OK  "
					CH_PS1_CIRCLE_BUTTON ": Back";

				// Wide enough for the title and a small margin past "Back",
				// rather than the old fixed-ish width that left a large empty
				// gap down the right-hand side.
				int boxWidth = getStringWidth(titleLine) + 14;
				int boxX     = (320 - boxWidth) / 2;
				int boxH     = 106;

				// A panel has to be readable first and pretty second, so this
				// deliberately does NOT use drawGlassPanel(): its sheen band
				// and brighter title bar split the background into two tones
				// behind the text, and white text over the lighter one was
				// hard to read.
				//
				// Instead: one flat tint at roughly 75% opacity. Blending is
				// a fixed 50% mix on this hardware, so drawing the same rect
				// twice halves the remaining transparency each time - two
				// passes is enough to settle the text background down while
				// still letting a hint of the wallpaper through.
				drawGlassCard(ctx, boxX, 50, boxWidth, boxH, mcAccent);

				printString(ctx, boxX + 8, 54, 0xffffff, titleLine);

				const char *labels[MENU_NUM_OPTIONS];
				labels[MENU_OPTION_COPY]   = "Copy";
				labels[MENU_OPTION_DELETE] = (sel && sel->deleted) ? "Undelete" : "Delete";
				labels[MENU_OPTION_CLEAR]  = "Clean";
				labels[MENU_OPTION_FORMAT] = "Format";
				labels[MENU_OPTION_CANCEL] = "Cancel";

				int drawRow = 0;
				for (int i = 0; i < MENU_NUM_OPTIONS; i++) {
					if ((i == MENU_OPTION_CLEAR) && !showClean)
						continue;
					printString(
						ctx, boxX + 8, 74 + drawRow * 16,
						(i == menuSelected) ? 0x1256e3 : 0xffffff,
						labels[i]
					);
					drawRow++;
				}
			} else if (stage == STAGE_CONFIRM) {
				// Same flat card as the options popup, but the title band
				// stays red: this dialog destroys data and should not blend
				// into the theme.
				drawGlassCard(ctx, 70, 85, 200, 60, mcAccent);
				drawRect(ctx, 71, 86, 198, 15, 0x2020c0, false);
				if (menuSelected == MENU_OPTION_FORMAT) {
					printString(ctx, 78, 89, 0xffffff, "Format this card?");
					printString(ctx, 78, 107, 0xffffff, "Erases ALL saves!");
				} else if (menuSelected == MENU_OPTION_CLEAR) {
					printString(ctx, 78, 89, 0xffffff, "Clear this block?");
					printString(ctx, 78, 107, 0xffffff, "Cannot be undeleted after!");
				} else if (entries[activeSlot][selected].deleted) {
					printString(ctx, 78, 89, 0xffffff, "Undelete this save?");
				} else {
					printString(ctx, 78, 89, 0xffffff, "Delete this save?");
				}
				printString(ctx, 78, 125, 0xffffff,
					CH_PS1_CROSS_BUTTON ": confirm   "
					CH_PS1_CIRCLE_BUTTON ": cancel");
			} else {
				printString(ctx, 16, 202, 0x505050, "D-PAD select   X: options");
				if (present[0] && present[1])
					printString(ctx, 16, 212, 0x505050, "L1/R1: switch card   START: refresh");
				else
					printString(ctx, 16, 212, 0x505050, "START: refresh cards");
			}
		}

		printString(ctx, 16, 220, 0x505050, "START+SELECT: return to menu");

		endFrame(ctx);
	}

	// Wait for any pending VRAM upload DMA transfer to fully finish
	// before returning - sendVRAMData() (used by every icon texture
	// upload on this screen) starts an asynchronous transfer and
	// returns without waiting for it to complete, only waiting for the
	// PREVIOUS transfer at the start of the NEXT call. With nothing
	// here to wait for the very last one, it could still be actively
	// running in the background when we hand control back - a real,
	// confirmed mechanism for exactly the kind of state corruption
	// reported earlier (background scroll stopping, lost menu
	// navigation sound).
	waitForGPUDMADone();

	// Flush button state before handing control back to the outer menu
	// system. The outer UIState's lastButtons is only updated inside
	// updateMenu(), which never runs while we're in this screen's own
	// loop above - so it's been frozen since before this screen was
	// entered. If we return while START (from the START+SELECT exit
	// combo) is still physically held, the outer menu compares it
	// against that stale lastButtons, sees it as a brand-new press, and
	// - since the cursor is still correctly sitting on this same menu
	// item - immediately re-triggers it as a "confirm selection," which
	// re-opens this exact screen. Symptom: exiting looks like it just
	// resets/reopens the screen, and only a *second* START+SELECT press
	// actually gets back to the menu. Waiting here for a full release
	// (mirroring the same flush already done on entry) guarantees the
	// outer menu sees zero buttons on its next update, regardless of how
	// stale its own state is.
	while (pollController(0) | pollController(1))
		;

	// Deliberately no enterToolsMenu()/enterMainMenu() call here -
	// state->currentMenu and state->menuCursor were never touched
	// anywhere in this function (this screen renders through its own
	// self-contained loop instead), so they still correctly point at
	// whichever menu and item were selected to get here. Overwriting
	// them was resetting the cursor to that menu's first item on every
	// exit instead of returning to the exact spot the user came from.
}
