/*
 * PSX-iTests - SIO (serial) EXE loader (see sio_loader.h)
 *
 * This implements the protocol used by the supplied UniROM 8.0.K image and
 * its PC-side NoPS (NOTPSXSerial) tool. The relevant code in that image is:
 *
 *   0x801D9C7C  SIO1 init (115200 baud, 8N2)
 *   0x801D9B98  blocking byte receive
 *   0x801DABE4  OKV2 / UPV2 / OKAY protocol negotiation
 *   0x801D7AB4  "SEXE" command and PS-EXE loader
 *   0x801D70F0  2048-byte corrective receive loop
 *
 * The wire format is not a raw PS-EXE stream. NoPS first sends "SEXE",
 * negotiates protocol V2, then sends the 2048-byte PS-EXE header followed by
 * entry/load/size/checksum metadata. The body is transferred in 2048-byte
 * chunks. At each chunk boundary the console sends "CHEK", receives the
 * host's byte-sum, and replies "MORE" or "ERR!". That flow control is also
 * what makes it safe to refresh the progress display: the host is stopped
 * while the GPU frame is being drawn, so the tiny SIO1 RX FIFO cannot
 * overflow.
 *
 * A raw-stream fallback is retained for older/simple senders. It receives
 * the standard header and payload without corrective acknowledgements, and
 * therefore uses the non-VSync drawing path during progress updates.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "common/gpu.h"
#include "common/sio0.h"
#include "main/defs.h"
#include "main/font.h"
#include "main/handoff.h"
#include "main/renderer.h"
#include "main/sio_loader.h"
#include "main/sound.h"
#include "ps1/cache.h"
#include "ps1/registers.h"

/* ---- SIO1 registers (KSEG1 / uncached) --------------------------------- */
#define SIO1_DATA (*(volatile uint8_t  *) 0xBF801050)
#define SIO1_STAT (*(volatile uint8_t  *) 0xBF801054)
#define SIO1_MODE (*(volatile uint16_t *) 0xBF801058)
#define SIO1_CTRL (*(volatile uint16_t *) 0xBF80105A)
#define SIO1_BAUD (*(volatile uint16_t *) 0xBF80105E)

#define SIO_STAT_TXRDY 0x05   /* TX-not-full OR TX-idle, as used by UniROM */
#define SIO_STAT_RXRDY 0x02
#define SIO_CTRL_ACK   0x0010

/* Per-byte spin budget for the header/body read loops: generous enough to
 * never trip during a normal transfer (including a slow USB-serial adapter's
 * write latency), finite enough that a genuinely dead link is reported
 * instead of hanging forever with no way back but a power cycle. */
#define BYTE_TIMEOUT 4000000u

/* Short timeout for the initial "has anything arrived yet" poll, so the
 * waiting screen stays responsive to Circle. */
#define WAIT_TIMEOUT 4000u

#define NEGOTIATION_TIMEOUT 4000000u
#define PROTOCOL_CHUNK_SIZE 2048u

/* Eleven 2 KB chunks take a little over two seconds at 115200 baud, 8N2. */
#define REFRESH_BYTES (PROTOCOL_CHUNK_SIZE * 11u)

/* ---- SIO1 primitives (byte-for-byte UniROM's) -------------------------- */

/* UniROM's init at 0x801D9C7C: CTRL 0x40, BAUD 0x12, MODE 0xCE, CTRL 0x05. */
static void sioInit(void) {
	SIO1_CTRL = 0x0040;
	SIO1_BAUD = 0x0012;
	SIO1_MODE = 0x00CE;
	SIO1_CTRL = 0x0005;   /* TXEN | RXEN */
}

/* Acknowledge, then read - UniROM's exact order at 0x801D9B98/0x801D9B48:
 * the acknowledge (CTRL bit 4, a write-only strobe that reads back as 0) is
 * a read-modify-write applied *before* SIO1_DATA is read. Doing it after (the
 * very first version of this file) leaves a latched framing/overrun error
 * standing while the byte is taken, which stops RXRDY from ever coming back
 * for the next byte. */
static inline uint8_t sioTakeByte(void) {
	SIO1_CTRL |= SIO_CTRL_ACK;
	return SIO1_DATA;
}

/* Blocking single-byte read with a spin budget. timeout == 0 means "wait
 * forever" (only used once a transfer is definitely already under way).
 * Returns -1 on timeout. */
static int sioGetc(unsigned timeout) {
	while (!(SIO1_STAT & SIO_STAT_RXRDY)) {
		if (timeout && --timeout == 0)
			return -1;
	}
	return (int) sioTakeByte();
}

static int sioPutc(uint8_t value, unsigned timeout) {
	while (!(SIO1_STAT & SIO_STAT_TXRDY)) {
		if (timeout && --timeout == 0)
			return 0;
	}
	SIO1_DATA = value;
	return 1;
}

static int sioWriteTag(const char tag[4]) {
	for (int i = 0; i < 4; i++) {
		if (!sioPutc((uint8_t) tag[i], BYTE_TIMEOUT))
			return 0;
	}
	return 1;
}

static int sioReadTag(char tag[4], unsigned timeout) {
	for (int i = 0; i < 4; i++) {
		int value = sioGetc(timeout);
		if (value < 0)
			return 0;
		tag[i] = (char) value;
	}
	return 1;
}

static int sioReadU32(uint32_t *value) {
	uint32_t result = 0;
	for (int i = 0; i < 4; i++) {
		int byte = sioGetc(BYTE_TIMEOUT);
		if (byte < 0)
			return 0;
		result |= (uint32_t) (uint8_t) byte << (i * 8);
	}
	*value = result;
	return 1;
}

/* Throw away anything already sitting in the FIFO (leftover TTY-redirect
 * chatter, a half-finished previous attempt) and clear any latched error
 * state so a fresh attempt starts clean. */
static void sioDrain(void) {
	unsigned guard = 4096;
	while ((SIO1_STAT & SIO_STAT_RXRDY) && --guard)
		(void) sioTakeByte();
	SIO1_CTRL = 0x0005 | SIO_CTRL_ACK;
}

/* End of this executable's image, from the linker (cmake/executable.ld). */
static uint32_t imageEndAddress(void) {
	uint32_t addr;
	__asm__ volatile(
		"lui   %0, %%hi(_imageEnd)\n"
		"addiu %0, %0, %%lo(_imageEnd)\n"
		: "=r"(addr)
	);
	return addr;
}

#define RAM_TOP 0x80200000u   /* stock 2 MB console; deliberately conservative */

/* ---- UI helpers --------------------------------------------------------
 *
 * drawLoaderScreen() is the normal, VSync-synced draw: used before and after
 * a transfer, when nothing is coming in over SIO1 and there is no harm in
 * blocking. drawLoaderScreenFast() is the same drawing code but skips the
 * wait, for use *during* a transfer where every millisecond of CPU time
 * risks the 8-byte RX FIFO overflowing.
 */

static void loaderRect(RenderContext *ctx, int x, int y, int w, int h, uint32_t color) {
	GPUDMAChain *chain = getCurrentChain(ctx);
	uint32_t *ptr = allocateGP0Packet(chain, 3);
	ptr[0] = color | gp0_rectangle(false, false, false);
	ptr[1] = gp0_xy(x, y);
	ptr[2] = gp0_xy(w, h);
}

static void buildLoaderScreen(
	RenderContext *ctx, const char *status, int havePercent, int percent,
	int showCancel
) {
	int w = ctx->screenWidth;

	loaderRect(ctx, 0, 0, ctx->screenWidth, ctx->screenHeight, gp0_rgb(0, 0, 0));

	printString(ctx, w / 2 - 33, 40, 0xffffff, "LOADING EXE");
	if (status)
		printString(ctx, 16, 70, 0x808080, status);

	if (havePercent) {
		int barX = 40, barY = 110, barW = w - 80, barH = 12;
		loaderRect(ctx, barX, barY, barW, barH, 0x202028);

		int fill = barW * percent / 100;
		if (fill < 0) fill = 0;
		if (fill > barW) fill = barW;
		if (fill > 0)
			loaderRect(ctx, barX, barY, fill, barH, 0x40C060);

		loaderRect(ctx, barX, barY, barW, 1, 0x505058);
		loaderRect(ctx, barX, barY + barH - 1, barW, 1, 0x505058);

		char pct[16];
		snprintf(pct, sizeof(pct), "%d%%", percent);
		printString(ctx, w / 2 - 12, barY + barH + 8, 0xc0c0c0, pct);
	}

	if (showCancel) {
		printString(ctx, 16, ctx->screenHeight - 26, 0x606060,
			CH_PS1_CIRCLE_BUTTON " Cancel");
	}
}

/* Normal draw: safe any time nothing is being received. */
static void drawLoaderScreen(
	RenderContext *ctx, const char *status, int havePercent, int percent,
	int showCancel
) {
	beginFrame(ctx);
	buildLoaderScreen(ctx, status, havePercent, percent, showCancel);
	endFrame(ctx);
}

/* Fast draw for use mid-transfer: same GP0 build-up, but skips
 * waitForVSync(). sendGPULinkedList() only starts the DMA and returns, so
 * this costs microseconds rather than up to 16.7 ms - short enough that it
 * cannot overrun the 8-byte SIO1 RX FIFO at 115200 baud. May tear on
 * screen; that is an acceptable trade during a loading bar. */
static void drawLoaderScreenFast(
	RenderContext *ctx, const char *status, int havePercent, int percent,
	int showCancel
) {
	GPUDMAChain *chain = getCurrentChain(ctx);
	chain->nextPacket   = chain->data;

	int bufferX = (ctx->frameCounter % 2) ? ctx->screenWidth : 0;
	GPU_GP1 = gp1_fbOffset(bufferX, 0);

	uint32_t *ptr = allocateGP0Packet(chain, 4);
	ptr[0] = gp0_setPage(0, true, false);
	ptr[1] = gp0_fbOffset1(bufferX, 0);
	ptr[2] = gp0_fbOffset2(bufferX + ctx->screenWidth - 1, ctx->screenHeight - 1);
	ptr[3] = gp0_fbOrigin(bufferX, 0);

	buildLoaderScreen(ctx, status, havePercent, percent, showCancel);

	*(chain->nextPacket) = gp0_endTag(0);
	waitForGP0Ready();
	sendGPULinkedList(chain->data);   // kicks off DMA; does not wait for it

	ctx->frameCounter++;
}

/* Show a message with a normal (synced) draw and sit on it until the user
 * presses Circle. Only ever called once a transfer has stopped. */
static void loaderMessage(RenderContext *ctx, const char *msg) {
	while (pollController(0) | pollController(1))
		;
	for (;;) {
		drawLoaderScreen(ctx, msg, 0, 0, 1);
		if ((pollController(0) | pollController(1)) & PAD_BTN_CIRCLE)
			break;
	}
	playCancelSound();
	while (pollController(0) | pollController(1))
		;
}

/* ---- main loader ------------------------------------------------------- */

void runSIOLoader(RenderContext *ctx, UIState *state, const MenuItem *item) {
	(void) state;
	(void) item;

	static uint8_t header[2048];
	enum {
		WIRE_RAW,
		WIRE_V2
	} wireMode;

	sioInit();
	sioDrain();

	while (pollController(0) | pollController(1))
		;

	/* --- wait-for-host phase ---------------------------------------------
	 * NoPS sends the four-byte SEXE command. A leading P is still accepted
	 * as a compatibility path for the old raw-stream helper. */
	int redraw = 0;
	int first  = -1;

	drawLoaderScreen(ctx, "Waiting for host...", 0, 0, 1);
	for (;;) {
		if ((pollController(0) | pollController(1)) & PAD_BTN_CIRCLE) {
			playCancelSound();
			while (pollController(0) | pollController(1))
				;
			return;
		}

		first = sioGetc(WAIT_TIMEOUT);
		if (first >= 0)
			break;

		if (++redraw >= 30) {
			redraw = 0;
			drawLoaderScreen(ctx, "Waiting for host...", 0, 0, 1);
		}
	}

	int headerOffset;
	uint32_t entry = 0;
	uint32_t loadAddr = 0;
	uint32_t loadSize = 0;
	uint32_t expectedChecksum = 0;

	if (first == 'S') {
		char command[4];
		char response[4];
		command[0] = 'S';

		for (int i = 1; i < 4; i++) {
			int byte = sioGetc(BYTE_TIMEOUT);
			if (byte < 0) {
				loaderMessage(ctx, "Serial command timed out.");
				return;
			}
			command[i] = (char) byte;
		}

		if (memcmp(command, "SEXE", 4) != 0) {
			loaderMessage(ctx, "Unknown serial command.");
			return;
		}

		/* This is UniROM's V2 challenge/response. Keep the screen update
		 * before OKAY: NoPS is waiting here and cannot overrun the RX FIFO. */
		if (!sioWriteTag("OKV2") ||
		    !sioReadTag(response, NEGOTIATION_TIMEOUT) ||
		    memcmp(response, "UPV2", 4) != 0) {
			loaderMessage(ctx, "NoPS V2 handshake failed.");
			return;
		}

		drawLoaderScreen(ctx, "Receiving header...", 0, 0, 0);
		waitForGPUDMADone();
		if (!sioWriteTag("OKAY")) {
			loaderMessage(ctx, "Serial transmit timed out.");
			return;
		}

		wireMode = WIRE_V2;
		headerOffset = 0;
	} else if (first == 'P') {
		/* Legacy helper: the first byte is already byte zero of PS-X EXE.
		 * Do not perform a synced redraw now; the host is already sending. */
		wireMode = WIRE_RAW;
		header[0] = (uint8_t) first;
		headerOffset = 1;
	} else {
		loaderMessage(ctx, "Expected SEXE or a PS-EXE.");
		return;
	}

	/* --- receive the 2048-byte PS-EXE header ------------------------------
	 * No redraw in this loop: after OKAY the sender streams immediately. */
	for (int i = headerOffset; i < (int) sizeof(header); i++) {
		int b = sioGetc(BYTE_TIMEOUT);
		if (b < 0) {
			char msg[48];
			snprintf(msg, sizeof(msg), "Timed out at header byte %d", i);
			loaderMessage(ctx, msg);
			return;
		}
		header[i] = (uint8_t) b;
	}

	/* NoPS follows the header with four little-endian values. Read them
	 * immediately, before doing any validation or drawing. */
	if (wireMode == WIRE_V2) {
		if (!sioReadU32(&entry) ||
		    !sioReadU32(&loadAddr) ||
		    !sioReadU32(&loadSize) ||
		    !sioReadU32(&expectedChecksum)) {
			loaderMessage(ctx, "Timed out reading EXE metadata.");
			return;
		}
	}

	const PSEXEHeader *hdr = (const PSEXEHeader *) header;

	if (memcmp(hdr->magic, "PS-X EXE", 8) != 0) {
		loaderMessage(ctx, "Not a PS-EXE - aborted.");
		return;
	}

	if (wireMode == WIRE_RAW) {
		entry = hdr->pc;
		loadAddr = hdr->textAddr;
		loadSize = hdr->textSize;
	} else if (entry != hdr->pc || loadAddr != hdr->textAddr) {
		loaderMessage(ctx, "EXE metadata does not match header.");
		return;
	}

	uint32_t loadPhysical = loadAddr & 0x1fffffffu;
	uint32_t entryPhysical = entry & 0x1fffffffu;

	if (loadPhysical < 0x10000u || loadPhysical >= 0x200000u ||
	    entryPhysical < 0x10000u || entryPhysical >= 0x200000u ||
	    loadSize == 0 || loadSize > 0x200000u - loadPhysical) {
		loaderMessage(ctx, "Bad load address/size - aborted.");
		return;
	}

	/* Always execute/copy through cached KSEG0, matching normal PS-EXE
	 * startup even if a sender supplied the physical form of the address. */
	loadAddr = 0x80000000u | loadPhysical;
	entry = 0x80000000u | entryPhysical;

	/* --- staging area ------------------------------------------------------
	 * Receive above our own image, never straight to loadAddr: a PS-EXE that
	 * links at 0x80010000 (most of them) would otherwise overwrite this very
	 * function while we were still reading it in. Moved into its real place
	 * only after the transfer is complete and quiesceForHandoff() has run. */
	uint32_t stageBase = (imageEndAddress() + 0x1000u) & ~0xfu;
	uint32_t capacity  = (stageBase < RAM_TOP) ? (RAM_TOP - stageBase) : 0;

	if (loadSize > capacity) {
		char msg[48];
		snprintf(msg, sizeof(msg), "EXE too big: %u KB, %u KB free",
			(unsigned) (loadSize / 1024), (unsigned) (capacity / 1024));
		loaderMessage(ctx, msg);
		return;
	}

	/* --- stream the program image into the staging area -------------------
	 * V2 checks every 2048-byte chunk. NoPS waits after receiving CHEK, which
	 * gives us a safe point to draw before replying MORE. A bad chunk gets
	 * ERR! and is overwritten by the retransmission at the same stage offset.
	 * Raw mode has no flow control, so its progress updates use the fast,
	 * non-VSync path. */
	volatile uint8_t *stage = (volatile uint8_t *) stageBase;
	uint32_t nextRefresh = 0;
	uint32_t transferred = 0;
	uint32_t totalChecksum = 0;

	while (transferred < loadSize) {
		uint32_t chunkSize = loadSize - transferred;
		if (chunkSize > PROTOCOL_CHUNK_SIZE)
			chunkSize = PROTOCOL_CHUNK_SIZE;

		for (;;) {
			uint32_t chunkChecksum = 0;

			for (uint32_t i = 0; i < chunkSize; i++) {
				int b = sioGetc(BYTE_TIMEOUT);
				if (b < 0) {
					char msg[48];
					snprintf(msg, sizeof(msg), "Timed out at EXE byte %u",
						(unsigned) (transferred + i));
					loaderMessage(ctx, msg);
					return;
				}
				stage[transferred + i] = (uint8_t) b;
				chunkChecksum += (uint8_t) b;
			}

			if (wireMode == WIRE_V2) {
				uint32_t hostChecksum;

				if (!sioWriteTag("CHEK") || !sioReadU32(&hostChecksum)) {
					loaderMessage(ctx, "Chunk checksum handshake failed.");
					return;
				}

				if (hostChecksum != chunkChecksum) {
					if (!sioWriteTag("ERR!")) {
						loaderMessage(ctx, "Serial transmit timed out.");
						return;
					}
					continue;
				}
			}

			uint32_t accepted = transferred + chunkSize;
			totalChecksum += chunkChecksum;

			if (accepted >= nextRefresh || accepted == loadSize) {
				int percent = (int) ((uint64_t) accepted * 100 / loadSize);
				if (wireMode == WIRE_V2) {
					/* NoPS is waiting for MORE, so a synced redraw is safe. */
					drawLoaderScreen(ctx, "Receiving EXE...", 1, percent, 0);
					waitForGPUDMADone();
				} else {
					drawLoaderScreenFast(ctx, "Receiving EXE...", 1, percent, 0);
				}
				nextRefresh = accepted + REFRESH_BYTES;
			}

			if (wireMode == WIRE_V2 && !sioWriteTag("MORE")) {
				loaderMessage(ctx, "Serial transmit timed out.");
				return;
			}

			transferred = accepted;
			break;
		}
	}

	if (wireMode == WIRE_V2 && totalChecksum != expectedChecksum) {
		loaderMessage(ctx, "Whole-file checksum mismatch.");
		return;
	}

	/* Final frame: transfer is over, nothing more coming in, safe to use the
	 * normal synced draw again. */
	drawLoaderScreen(ctx, "Launching...", 1, 100, 0);

	/*
	 * Detection and transfer above are intentionally identical to the
	 * hardware-proven build. Only after NoPS has completed do we enter the
	 * shared clean-RAM handoff.
	 */
	PSEXEHeader launchHeader = *hdr;
	launchHeader.pc = entry;
	launchHeader.textAddr = loadAddr;
	launchStagedPSEXE(
		&launchHeader,
		(const uint8_t *) stageBase,
		loadSize
	);
}
