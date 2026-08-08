/*
 * PSX-iTests - persistent dashboard settings
 *
 * This deliberately uses the normal PlayStation save layout so the entry is
 * visible to the BIOS and to the project's own Memory Card Manager:
 *
 *   directory sector 1..15 : allocated one-block file entry
 *   block sector + 0       : "SC" title frame, title and 16-colour CLUT
 *   block sector + 1       : 16x16 4bpp icon
 *   block sector + 2       : checksummed/versioned settings record
 *
 * The content sectors are written and verified before the directory entry is
 * committed.  A new save therefore never advertises a half-written file.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "common/sio0.h"
#include "main/defs.h"
#include "main/font.h"
#include "main/hud.h"
#include "main/memcard.h"
#include "main/renderer.h"
#include "main/settings_save.h"
#include "main/sound.h"
#include "main/xmb_bg.h"

extern const uint8_t settingsSaveIconTexture[];
extern const uint8_t settingsSaveIconPalette[];

#define MC_BLOCK_COUNT          15
#define MC_SECTORS_PER_BLOCK    64
#define MC_SECTOR_SIZE          128
#define MC_STATE_FREE           0xa0
#define MC_STATE_USED_FIRST     0x51
#define SETTINGS_VERSION        1

/* 20 bytes, the traditional region/product/name form used by PS1 saves. */
static const char settingsFilename[21] = "BASLUS-90000PSXITEST";
static const uint8_t settingsMagic[8] = {
	'P', 'S', 'X', 'S', 'E', 'T', '0', '1'
};

enum {
	FLAG_BGM_ENABLED = 1 << 0,
	FLAG_SFX_ENABLED = 1 << 1,
	FLAG_BG_SCROLL   = 1 << 2,
	FLAG_SYSTEM_HUD  = 1 << 3
};

typedef enum {
	CARD_RESULT_OK,
	CARD_RESULT_NO_CARD,
	CARD_RESULT_NOT_FOUND,
	CARD_RESULT_FULL,
	CARD_RESULT_INVALID,
	CARD_RESULT_IO_ERROR
} CardResult;

/* Static scratch keeps the PS1 executable's deliberately small stack safe. */
static uint8_t sector[MC_SECTOR_SIZE];
static uint8_t verifySector[MC_SECTOR_SIZE];

static uint8_t xorChecksum(const uint8_t data[MC_SECTOR_SIZE]) {
	uint8_t checksum = 0;
	for (int i = 0; i < MC_SECTOR_SIZE - 1; i++)
		checksum = (uint8_t) (checksum ^ data[i]);
	return checksum;
}

static bool filenameMatches(const uint8_t dir[MC_SECTOR_SIZE]) {
	return memcmp(&dir[0x0a], settingsFilename, 20) == 0;
}

static bool writeAndVerify(int port, uint16_t address, const uint8_t *data) {
	if (!memoryCardWriteSector(port, address, data))
		return false;
	if (!memoryCardReadSector(port, address, verifySector))
		return false;
	return memcmp(data, verifySector, MC_SECTOR_SIZE) == 0;
}

static int findSaveBlock(int port, int *firstFree, bool *readFailed) {
	*firstFree = 0;
	*readFailed = false;

	for (int block = 1; block <= MC_BLOCK_COUNT; block++) {
		if (!memoryCardReadSector(port, (uint16_t) block, sector)) {
			*readFailed = true;
			return 0;
		}

		if (sector[0] == MC_STATE_USED_FIRST && filenameMatches(sector))
			return block;
		if (!*firstFree && sector[0] == MC_STATE_FREE)
			*firstFree = block;
	}

	return 0;
}

static void captureSettings(uint8_t out[MC_SECTOR_SIZE]) {
	memset(out, 0, MC_SECTOR_SIZE);
	memcpy(out, settingsMagic, sizeof(settingsMagic));
	out[8]  = SETTINGS_VERSION;
	out[9]  = xmbThemeIndex;
	out[10] = xmbPaletteIndex;
	out[11] = xmbIconStyle;
	out[12] = xmbWaveStyle;
	out[13] = (uint8_t) getBGMIndex();
	out[14] = (uint8_t) getSFXSetIndex();
	out[15] =
		(isBGMEnabled()              ? FLAG_BGM_ENABLED : 0) |
		(isSFXEnabled()              ? FLAG_SFX_ENABLED : 0) |
		(isBackgroundScrollEnabled() ? FLAG_BG_SCROLL   : 0) |
		(isSystemHUDEnabled()        ? FLAG_SYSTEM_HUD  : 0);
	out[127] = xorChecksum(out);
}

static bool validSettings(const uint8_t data[MC_SECTOR_SIZE]) {
	if (memcmp(data, settingsMagic, sizeof(settingsMagic)) != 0 ||
	    data[8] != SETTINGS_VERSION || data[127] != xorChecksum(data))
		return false;

	return data[9]  < XMB_THEME_COUNT &&
	       data[10] < XMB_PALETTE_COUNT &&
	       data[11] < XMB_ICON_STYLE_COUNT &&
	       data[12] < XMB_WAVE_STYLE_COUNT &&
	       data[13] < getBGMCount() && bgmTrackAvailable(data[13]) &&
	       data[14] < getSFXSetCount();
}

static void applySettings(const uint8_t data[MC_SECTOR_SIZE]) {
	uint8_t flags = data[15];

	xmbThemeIndex   = data[9];
	xmbPaletteIndex = data[10];
	xmbIconStyle    = data[11];
	xmbWaveStyle    = data[12];
	selectBGM(data[13]);
	selectSFXSet(data[14]);

	bool wantBGM = (flags & FLAG_BGM_ENABLED) != 0;
	bool wantSFX = (flags & FLAG_SFX_ENABLED) != 0;
	bool wantHUD = (flags & FLAG_SYSTEM_HUD) != 0;
	if (isBGMEnabled() != wantBGM)
		toggleBGM();
	if (isSFXEnabled() != wantSFX)
		toggleSFX();
	setBackgroundScrollEnabled((flags & FLAG_BG_SCROLL) != 0);
	if (isSystemHUDEnabled() != wantHUD)
		toggleSystemHUD();
}

static CardResult saveToCard(int port) {
	if (!memoryCardPresent(port))
		return CARD_RESULT_NO_CARD;

	int firstFree;
	bool readFailed;
	int block = findSaveBlock(port, &firstFree, &readFailed);
	if (readFailed)
		return CARD_RESULT_IO_ERROR;
	if (!block)
		block = firstFree;
	if (!block)
		return CARD_RESULT_FULL;

	uint16_t base = (uint16_t) (block * MC_SECTORS_PER_BLOCK);

	/* Standard title frame. ASCII is valid within Shift-JIS for this title. */
	memset(sector, 0, MC_SECTOR_SIZE);
	sector[0] = 'S';
	sector[1] = 'C';
	sector[2] = 0x11; /* one static icon frame */
	memcpy(&sector[4], "PSX-iTests Settings", 19);
	memcpy(&sector[0x60], settingsSaveIconPalette, 32);
	if (!writeAndVerify(port, base, sector))
		return CARD_RESULT_IO_ERROR;

	memcpy(sector, settingsSaveIconTexture, MC_SECTOR_SIZE);
	if (!writeAndVerify(port, (uint16_t) (base + 1), sector))
		return CARD_RESULT_IO_ERROR;

	captureSettings(sector);
	if (!writeAndVerify(port, (uint16_t) (base + 2), sector))
		return CARD_RESULT_IO_ERROR;

	/* Commit the directory last so a brand-new partial save stays invisible. */
	memset(sector, 0, MC_SECTOR_SIZE);
	sector[0] = MC_STATE_USED_FIRST;
	sector[4] = 0x00;
	sector[5] = 0x20; /* 8192 bytes, little endian */
	sector[8] = 0xff;
	sector[9] = 0xff;
	memcpy(&sector[0x0a], settingsFilename, 20);
	sector[127] = xorChecksum(sector);
	if (!writeAndVerify(port, (uint16_t) block, sector))
		return CARD_RESULT_IO_ERROR;

	return CARD_RESULT_OK;
}

static CardResult loadFromCard(int port, bool apply) {
	if (!memoryCardPresent(port))
		return CARD_RESULT_NO_CARD;

	int firstFree;
	bool readFailed;
	int block = findSaveBlock(port, &firstFree, &readFailed);
	(void) firstFree;
	if (readFailed)
		return CARD_RESULT_IO_ERROR;
	if (!block)
		return CARD_RESULT_NOT_FOUND;

	uint16_t dataSector = (uint16_t) (block * MC_SECTORS_PER_BLOCK + 2);
	if (!memoryCardReadSector(port, dataSector, sector))
		return CARD_RESULT_IO_ERROR;
	if (!validSettings(sector))
		return CARD_RESULT_INVALID;
	if (apply)
		applySettings(sector);
	return CARD_RESULT_OK;
}

bool loadSettingsAtBoot(void) {
	for (int port = 0; port < 2; port++) {
		if (loadFromCard(port, true) == CARD_RESULT_OK)
			return true;
	}
	return false;
}

static const char *resultText(CardResult result, bool loading, int port) {
	static char message[48];
	switch (result) {
		case CARD_RESULT_OK:
			snprintf(message, sizeof(message), "%s settings on Slot %d",
				loading ? "Loaded" : "Saved", port + 1);
			break;
		case CARD_RESULT_NO_CARD:
			snprintf(message, sizeof(message), "No card in Slot %d", port + 1);
			break;
		case CARD_RESULT_NOT_FOUND:
			snprintf(message, sizeof(message), "No settings save in Slot %d", port + 1);
			break;
		case CARD_RESULT_FULL:
			snprintf(message, sizeof(message), "Slot %d has no free block", port + 1);
			break;
		case CARD_RESULT_INVALID:
			snprintf(message, sizeof(message), "Settings save is invalid");
			break;
		default:
			snprintf(message, sizeof(message), "Memory card I/O failed");
			break;
	}
	return message;
}

static void drawSettingsScreen(
	RenderContext *ctx, int selected, const bool present[2], const char *notice,
	const char *busy
) {
	static const char *const rows[5] = {
		"Save to Slot 1", "Load from Slot 1",
		"Save to Slot 2", "Load from Slot 2", "Back"
	};

	beginFrame(ctx);
	drawXMBBackground(ctx);
	printString(ctx, 16, 18, 0xffffff, "SAVE SETTINGS");
	printString(ctx, 16, 34, 0x707070,
		"Theme, music, SFX and display options");

	for (int i = 0; i < 5; i++) {
		int port = i / 2;
		uint32_t color = (i == selected) ? 0xffffff : 0x808080;
		if (i < 4 && !present[port])
			color = (i == selected) ? 0x707070 : 0x404040;

		char line[36];
		snprintf(line, sizeof(line), "%c %s", (i == selected) ? '>' : ' ', rows[i]);
		printString(ctx, 28, 64 + i * 20, color, line);
	}

	printString(ctx, 202, 64, present[0] ? 0x60d060 : 0x606060,
		present[0] ? "CARD READY" : "NO CARD");
	printString(ctx, 202, 104, present[1] ? 0x60d060 : 0x606060,
		present[1] ? "CARD READY" : "NO CARD");

	if (busy)
		printString(ctx, 28, 178, 0xffffff, busy);
	else if (notice)
		printString(ctx, 28, 178, 0x60d060, notice);

	printString(ctx, 16, 218, 0x606060,
		CH_PS1_DPAD " Navigate   " CH_PS1_CROSS_BUTTON " Confirm   "
		CH_PS1_START_BUTTON " Rescan   " CH_PS1_CIRCLE_BUTTON " Back");
	endFrame(ctx);
}

void runSettingsSave(RenderContext *ctx, UIState *state, const MenuItem *item) {
	(void) state;
	(void) item;

	while (pollController(0) | pollController(1))
		;

	bool present[2] = {
		memoryCardPresent(0), memoryCardPresent(1)
	};
	int selected = 0;
	uint16_t lastPad = 0;
	const char *notice = 0;

	for (;;) {
		uint16_t pad = pollController(0) | pollController(1);
		uint16_t pressed = pad & ~lastPad;
		lastPad = pad;

		if ((pressed & PAD_BTN_UP) && selected > 0) {
			selected--;
			playScrollSound();
		}
		if ((pressed & PAD_BTN_DOWN) && selected < 4) {
			selected++;
			playScrollSound();
		}
		if (pressed & PAD_BTN_START) {
			present[0] = memoryCardPresent(0);
			present[1] = memoryCardPresent(1);
			notice = "Memory cards rescanned";
			playConfirmSound();
		}
		if ((pressed & PAD_BTN_CIRCLE) ||
		    ((pressed & PAD_BTN_CROSS) && selected == 4)) {
			playCancelSound();
			break;
		}
		if ((pressed & PAD_BTN_CROSS) && selected < 4) {
			int port = selected / 2;
			bool loading = (selected & 1) != 0;
			drawSettingsScreen(
				ctx, selected, present, 0,
				loading ? "Loading from memory card..." : "Saving to memory card..."
			);

			CardResult result = loading
				? loadFromCard(port, true) : saveToCard(port);
			present[port] = memoryCardPresent(port);
			notice = resultText(result, loading, port);
			if (result == CARD_RESULT_OK)
				playConfirmSound();
			else
				playCancelSound();
		}

		drawSettingsScreen(ctx, selected, present, notice, 0);
	}

	while (pollController(0) | pollController(1))
		;
}
