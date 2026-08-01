/*
 * PSX-iTests - CD music player
 *
 * A basic CD-DA (audio track) player. Reuses the same low-level CD-ROM
 * command infrastructure already proven working for the CD-ROM
 * controller version test (BUSYSTS synchronization, FIFO-based response
 * reading) - self-contained here rather than shared, same reasoning as
 * every other test screen in this project.
 *
 * Audio routing fix (see history): CD-DA audio needs BOTH the CD Audio
 * Input Volume register (0x1F801DB0, "how loud") AND SPU_CTRL bit 0
 * ("CD Audio Enable", is it connected at all) - volume alone does
 * nothing without the enable bit also set. Read-modify-write is used on
 * SPU_CTRL throughout, since it also holds the SPU's own core enable/DAC
 * bits our BGM/SFX depend on.
 *
 * Reverb: a full software parametric EQ was considered and deliberately
 * NOT implemented - it would require abandoning the CD-DA hardware
 * auto-play pipeline entirely (reading raw PCM into RAM, fixed-point DSP
 * filtering every sample, then re-encoding to ADPCM in real time, since
 * SPU voices only play ADPCM), which is game-engine-level audio work,
 * not proportionate for this project. Reverb is a different story - the
 * SPU has genuine hardware reverb that can be applied directly to CD-DA
 * through the same pipeline that's already working, no custom DSP
 * needed. Preset values below are cross-referenced from multiple
 * independent sources (hitmen.c02.at's original hardware documentation,
 * an LV2 plugin that recreates this exact algorithm, and a commercial
 * plugin listing the same preset names) rather than taken from one place.
 *
 * IMPORTANT: PSX-iTests' own disc has no audio tracks on it at all - this
 * only does anything useful once you swap in an actual audio CD or
 * multi-track game disc after booting, same as how the BIOS's own music
 * player works.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "common/sio0.h"
#include "main/cd_player.h"
#include "main/defs.h"
#include "main/font.h"
#include "main/mainmenu.h"
#include "main/xmb_bg.h"
#include "main/sound.h"
#include "ps1/registers.h"

#define CDROM_REG0 (*(volatile uint8_t *) 0x1f801800)
#define CDROM_REG1 (*(volatile uint8_t *) 0x1f801801)
#define CDROM_REG2 (*(volatile uint8_t *) 0x1f801802)
#define CDROM_REG3 (*(volatile uint8_t *) 0x1f801803)

#define CDROM_STAT_RSLRRDY (1 << 5)
#define CDROM_STAT_BUSYSTS (1 << 7)

#define SPU_CD_VOL (*(volatile uint32_t *) 0x1f801db0)

// mBASE - Reverb Work Area Start Address, in 8-byte units. THIS WAS THE
// ACTUAL BUG in the first reverb attempt: this register was never set at
// all, so the reverb engine's buffer pointed at whatever garbage address
// was left over - almost certainly overlapping our own BGM/SFX sample
// data (which occupies nearly all of SPU RAM per this project's own
// layout, leaving only ~8KB free - nowhere near enough for these
// presets, which need hundreds of KB of buffer space based on their
// internal address offsets). Fix: explicitly reclaim BGM's SPU RAM
// region for the reverb buffer (safe, since BGM is paused during this
// screen anyway) by pointing mBASE right after the small system area,
// and fully re-upload all SPU samples via initSound()+playBGM() on exit
// - the same proven pattern already used elsewhere in this project
// (spu_channel_test.c, the RAM tester) whenever something else
// temporarily needs SPU RAM.
#define SPU_REVERB_BASE (*(volatile uint16_t *) 0x1f801da2)

static void cdromSendCommand(uint8_t cmd, const uint8_t *params, int numParams) {
	while (CDROM_REG0 & CDROM_STAT_BUSYSTS)
		;

	CDROM_REG0 = 0;
	CDROM_REG3 = 0xc0;

	for (int i = 0; i < numParams; i++)
		CDROM_REG2 = params[i];

	CDROM_REG0 = 1;
	CDROM_REG2 = 0x00;
	CDROM_REG3 = 0x07;
	CDROM_REG0 = 0;

	CDROM_REG1 = cmd;
}

static int cdromWaitForInterrupt(void) {
	while (CDROM_REG0 & CDROM_STAT_BUSYSTS)
		;

	CDROM_REG0 = 1;

	uint32_t timeout = 2000000;
	uint8_t  flags;
	do {
		flags = CDROM_REG3 & 0x07;
		if (--timeout == 0)
			return -1;
	} while (flags == 0);

	CDROM_REG3 = 0x07;
	return flags;
}

static int cdromReadResponse(uint8_t *out, int maxBytes) {
	CDROM_REG0 = 1;

	int count = 0;
	while ((count < maxBytes) && (CDROM_REG0 & CDROM_STAT_RSLRRDY))
		out[count++] = CDROM_REG1;

	return count;
}

static int cdromCommand(
	uint8_t cmd,
	const uint8_t *params,
	int numParams,
	uint8_t *response,
	int maxResponse
) {
	cdromSendCommand(cmd, params, numParams);

	int interrupt = cdromWaitForInterrupt();
	if (interrupt < 0)
		return -1;

	return cdromReadResponse(response, maxResponse);
}

#define CD_CMD_STOP    0x08
#define CD_CMD_PAUSE   0x09
#define CD_CMD_PLAY    0x03
#define CD_CMD_GETTN   0x13
#define CD_CMD_GETLOCP 0x11

typedef enum { CD_STOPPED, CD_PLAYING, CD_PAUSED } PlayState;

static uint8_t decToBcd(int value) {
	return (uint8_t) (((value / 10) << 4) | (value % 10));
}

static int bcdToDec(uint8_t bcd) {
	return ((bcd >> 4) * 10) + (bcd & 0x0f);
}

/* ---- Reverb presets ----
 *
 * 32 sequential 16-bit registers starting at 0x1F801DC0. Values
 * cross-referenced from multiple independent sources (see file header).
 */

typedef enum {
	REVERB_OFF,
	REVERB_ROOM,
	REVERB_STUDIO_SMALL,
	REVERB_STUDIO_MEDIUM,
	REVERB_STUDIO_LARGE,
	REVERB_COUNT
} ReverbPreset;

static const uint16_t REVERB_PRESETS[REVERB_COUNT][32] = {
	[REVERB_OFF] = {
		0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
		0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
		0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
		0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
	},
	[REVERB_ROOM] = {
		0x007d, 0x005b, 0x6d80, 0x54b8, 0xbed0, 0x0000, 0x0000, 0xba80,
		0x5800, 0x5300, 0x04d6, 0x0333, 0x03f0, 0x0227, 0x0374, 0x01ef,
		0x0334, 0x01b5, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
		0x0000, 0x0000, 0x01b4, 0x0136, 0x00b8, 0x005c, 0x8000, 0x8000
	},
	[REVERB_STUDIO_SMALL] = {
		0x0033, 0x0025, 0x70f0, 0x4fa8, 0xbce0, 0x4410, 0xc0f0, 0x9c00,
		0x5280, 0x4ec0, 0x03e4, 0x031b, 0x03a4, 0x02af, 0x0372, 0x0266,
		0x031c, 0x025d, 0x025c, 0x018e, 0x022f, 0x0135, 0x01d2, 0x00b7,
		0x018f, 0x00b5, 0x00b4, 0x0080, 0x004c, 0x0026, 0x8000, 0x8000
	},
	[REVERB_STUDIO_MEDIUM] = {
		0x00b1, 0x007f, 0x70f0, 0x4fa8, 0xbce0, 0x4510, 0xbef0, 0xb4c0,
		0x5280, 0x4ec0, 0x0904, 0x076b, 0x0824, 0x065f, 0x07a2, 0x0616,
		0x076c, 0x05ed, 0x05ec, 0x042e, 0x050f, 0x0305, 0x0462, 0x02b7,
		0x042f, 0x0265, 0x0264, 0x01b2, 0x0100, 0x0080, 0x8000, 0x8000
	},
	[REVERB_STUDIO_LARGE] = {
		0x00e3, 0x00a9, 0x6f60, 0x4fa8, 0xbce0, 0x4510, 0xbef0, 0xa680,
		0x5680, 0x52c0, 0x0dfb, 0x0b58, 0x0d09, 0x0a3c, 0x0bd9, 0x0973,
		0x0b59, 0x08da, 0x08d9, 0x05e9, 0x07ec, 0x04b0, 0x06ef, 0x03d2,
		0x05ea, 0x031d, 0x031c, 0x0238, 0x0154, 0x00aa, 0x8000, 0x8000
	}
};

static const char *REVERB_NAMES[REVERB_COUNT] = {
	"OFF", "ROOM", "STUDIO SMALL", "STUDIO MEDIUM", "STUDIO LARGE"
};

// Follows the documented setup sequence: disable reverb, zero the wet
// mix, load the new preset, then (if not OFF) re-enable and restore the
// wet mix. Doing this out of order is what causes noise/glitches.
static void applyReverbPreset(ReverbPreset preset) {
	SPU_CTRL &= ~(SPU_CTRL_REVERB_ENABLE | SPU_CTRL_I2SA_REVERB);
	SPU_EVOLL = 0;
	SPU_EVOLR = 0;

	// Reclaim SPU RAM past the small fixed system area for the reverb
	// buffer - safe because we fully re-upload BGM/SFX samples on exit.
	SPU_REVERB_BASE = 0x1000 / 8;

	volatile uint16_t *regs = (volatile uint16_t *) 0x1f801dc0;
	for (int i = 0; i < 32; i++)
		regs[i] = REVERB_PRESETS[preset][i];

	if (preset != REVERB_OFF) {
		SPU_CTRL  |= SPU_CTRL_REVERB_ENABLE | SPU_CTRL_I2SA_REVERB;
		SPU_EVOLL  = 0x2800;
		SPU_EVOLR  = 0x2800;
	}
}

#define GRID_COLS       5
#define GRID_MAX_ROWS   4
#define GRID_MAX_TRACKS (GRID_COLS * GRID_MAX_ROWS)

#define CD_VOL_STEP 0x300
#define CD_VOL_MAX  0x7fff

void runCDPlayer(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
) {
	(void) state;
	(void) item;

	uint16_t savedSpuCtrl = SPU_CTRL;
	SPU_CTRL = savedSpuCtrl | SPU_CTRL_I2SA_ENABLE;

	int cdVolume = CD_VOL_MAX;
	SPU_CD_VOL = ((uint32_t) cdVolume) | (((uint32_t) cdVolume) << 16);

	ReverbPreset reverbPreset = REVERB_OFF;
	applyReverbPreset(reverbPreset);

	bool bgmWasEnabled = isBGMEnabled();
	if (bgmWasEnabled)
		toggleBGM();

	uint8_t response[16];
	int     respLen = cdromCommand(CD_CMD_GETTN, NULL, 0, response, sizeof(response));

	bool haveDisc   = (respLen >= 3);
	int  firstTrack = haveDisc ? bcdToDec(response[1]) : 0;
	int  lastTrack  = haveDisc ? bcdToDec(response[2]) : 0;
	int  shownTracks = lastTrack;
	if (shownTracks > GRID_MAX_TRACKS)
		shownTracks = GRID_MAX_TRACKS;

	int       currentTrack = firstTrack;
	PlayState playState     = CD_STOPPED;
	uint16_t  lastButtons   = 0;
	int       elapsedMin    = 0;
	int       elapsedSec    = 0;
	int       discCheckTimer = 0;

	char line[64];

	while (pollController(0) | pollController(1))
		;

	for (;;) {
		uint16_t buttons = pollController(0) | pollController(1);
		uint16_t pressed = buttons & ~lastButtons;
		lastButtons       = buttons;

		if ((buttons & PAD_BTN_START) && (buttons & PAD_BTN_SELECT)) {
			playCancelSound();
			break;
		}

		// If no disc/tracks were found, keep retrying periodically (once
		// every ~30 frames) so inserting a disc after opening this
		// screen works without needing to back out and back in again.
		if (!haveDisc) {
			discCheckTimer++;
			if (discCheckTimer >= 30) {
				discCheckTimer = 0;

				int recheck = cdromCommand(
					CD_CMD_GETTN, NULL, 0, response, sizeof(response)
				);
				if (recheck >= 3) {
					haveDisc     = true;
					firstTrack   = bcdToDec(response[1]);
					lastTrack    = bcdToDec(response[2]);
					shownTracks  = lastTrack;
					if (shownTracks > GRID_MAX_TRACKS)
						shownTracks = GRID_MAX_TRACKS;
					currentTrack = firstTrack;
				}
			}
		}

		// Volume: continuous while held, not just on press.
		if (buttons & PAD_BTN_R2) {
			cdVolume += CD_VOL_STEP;
			if (cdVolume > CD_VOL_MAX)
				cdVolume = CD_VOL_MAX;
			SPU_CD_VOL = ((uint32_t) cdVolume) | (((uint32_t) cdVolume) << 16);
		}
		if (buttons & PAD_BTN_L2) {
			cdVolume -= CD_VOL_STEP;
			if (cdVolume < 0)
				cdVolume = 0;
			SPU_CD_VOL = ((uint32_t) cdVolume) | (((uint32_t) cdVolume) << 16);
		}

		if (pressed & PAD_BTN_TRIANGLE) {
			reverbPreset = (ReverbPreset) ((reverbPreset + 1) % REVERB_COUNT);
			applyReverbPreset(reverbPreset);
		}

		if (haveDisc) {
			int index = currentTrack - firstTrack;

			if (pressed & PAD_BTN_LEFT) {
				if (currentTrack > firstTrack)
					currentTrack--;
			}
			if (pressed & PAD_BTN_RIGHT) {
				if (currentTrack < lastTrack)
					currentTrack++;
			}
			if (pressed & PAD_BTN_UP) {
				if ((index - GRID_COLS) >= 0)
					currentTrack -= GRID_COLS;
			}
			if (pressed & PAD_BTN_DOWN) {
				if ((index + GRID_COLS) < shownTracks)
					currentTrack += GRID_COLS;
			}

			if (pressed & PAD_BTN_CROSS) {
				uint8_t param = decToBcd(currentTrack);
				cdromCommand(CD_CMD_PLAY, &param, 1, response, sizeof(response));
				playState = CD_PLAYING;
			}
			if (pressed & PAD_BTN_SQUARE) {
				cdromCommand(CD_CMD_PAUSE, NULL, 0, response, sizeof(response));
				playState = CD_PAUSED;
			}
			if (pressed & PAD_BTN_CIRCLE) {
				cdromCommand(CD_CMD_STOP, NULL, 0, response, sizeof(response));
				playState  = CD_STOPPED;
				elapsedMin = 0;
				elapsedSec = 0;
			}

			if (playState == CD_PLAYING) {
				uint8_t loc[16];
				int locLen = cdromCommand(
					CD_CMD_GETLOCP, NULL, 0, loc, sizeof(loc)
				);
				if (locLen >= 4) {
					elapsedMin = bcdToDec(loc[2]);
					elapsedSec = bcdToDec(loc[3]);
				}
			}
		}

		beginFrame(ctx);
		drawXMBBackground(ctx);

		printString(ctx, 16, 10, 0x808080, "CD MUSIC PLAYER");

		if (!haveDisc) {
			printString(ctx, 24, 44, 0x808080, "No disc / no tracks found");
			printString(ctx, 24, 56, 0x808080, "Insert an audio CD or game disc");
			printString(ctx, 24, 68, 0x505050, "(checking automatically...)");
		} else {
			for (int t = 0; t < shownTracks; t++) {
				int trackNum = firstTrack + t;
				int col      = t % GRID_COLS;
				int row      = t / GRID_COLS;
				int x        = 24 + col * 52;
				int y        = 26 + row * 20;

				bool isCurrent = (trackNum == currentTrack);
				bool isPlaying = isCurrent && (playState == CD_PLAYING);

				drawRect(
					ctx, x, y, 44, 16,
					isPlaying ? 0x1256e3 : (isCurrent ? 0x505050 : 0x282828),
					false
				);

				snprintf(line, sizeof(line), "%02d", trackNum);
				printString(ctx, x + 12, y + 4, 0xffffff, line);
			}

			if (lastTrack > GRID_MAX_TRACKS) {
				snprintf(
					line, sizeof(line), "(+%d more, use LEFT/RIGHT)",
					lastTrack - GRID_MAX_TRACKS
				);
				printString(ctx, 24, 108, 0x505050, line);
			}

			snprintf(
				line, sizeof(line), "TRACK %02d/%02d  TIME %02d:%02d",
				currentTrack, lastTrack, elapsedMin, elapsedSec
			);
			printString(ctx, 24, 122, 0xffffff, line);

			const char *stateText;
			switch (playState) {
				case CD_PLAYING: stateText = "> PLAYING"; break;
				case CD_PAUSED:  stateText = "|| PAUSED"; break;
				default:         stateText = "[] STOPPED"; break;
			}
			printString(ctx, 24, 134, 0x1256e3, stateText);

			snprintf(
				line, sizeof(line), "VOLUME %3d%%   REVERB: %s",
				(cdVolume * 100) / CD_VOL_MAX, REVERB_NAMES[reverbPreset]
			);
			printString(ctx, 24, 148, 0xffffff, line);

			printString(
				ctx, 16, 172, 0x505050,
				CH_PS1_CROSS_BUTTON " PLAY   "
				CH_PS1_SQUARE_BUTTON " PAUSE   "
				CH_PS1_CIRCLE_BUTTON " STOP   "
				CH_PS1_TRIANGLE_BUTTON " REVERB"
			);
			printString(ctx, 16, 184, 0x505050, "D-PAD select   R2/L2 volume");
		}

		printString(ctx, 16, 218, 0x505050, "START+SELECT: return to menu");

		endFrame(ctx);
	}

	cdromCommand(CD_CMD_STOP, NULL, 0, response, sizeof(response));
	SPU_CD_VOL = 0;
	applyReverbPreset(REVERB_OFF);
	SPU_CTRL = savedSpuCtrl;

	// Fully re-upload SPU samples - required because the reverb buffer
	// reclaimed and overwrote that same SPU RAM region while active, not
	// just a volume/mute restore.
	initSound();
	if (bgmWasEnabled)
		playBGM();

	// Flush button state before handing control back to the outer menu
	// system - see the matching comment in memcard.c's
	// runMemoryCardManager() for the full explanation. Without this, a
	// still-held START from the START+SELECT exit combo gets misread by
	// the outer menu as a fresh "confirm" press on this same
	// still-highlighted item, immediately re-opening this screen.
	while (pollController(0) | pollController(1))
		;

	// Deliberately no enterToolsMenu() call here - state->currentMenu
	// and state->menuCursor were never touched anywhere in this
	// function (this screen renders through its own self-contained
	// loop instead), so they still correctly point at whichever menu
	// and item were selected to get here.
}
