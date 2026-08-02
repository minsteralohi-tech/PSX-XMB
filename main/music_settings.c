/*
 * PSX-iTests - Music settings screen (see music_settings.h)
 *
 * SUPERSEDED: selecting "Music" under Settings now opens a flyout directly
 * in the XMB (see isMusicItem()/musicMenuOpen in xmb_menu.c), matching how
 * the theme customization menu already works, instead of navigating away to
 * this separate full-screen page - that was the actual complaint this
 * replaced ("takes me to a new page and menu disappears, I don't want it").
 * This function is consequently unreachable now (xmb_menu.c intercepts
 * Cross on the Music row before dispatch would ever call this), but is left
 * in place rather than deleted outright, since a clean removal also means
 * unpicking its CMakeLists.txt/header registration - small enough risk,
 * on a session that's already made a couple of mistakes doing exactly that
 * kind of multi-file edit, that it's better done as its own deliberate
 * follow-up than folded into this fix.
 *
 * The two-level design described below is preserved for reference, since
 * the XMB flyout follows the same X-to-select/O-to-back pattern, just
 * rendered by xmb_menu.c instead of this file:
 *
 *   category column (BGM, SFX)
 *     -> X opens the value column for whichever is highlighted
 *     -> Circle exits the whole screen, back to Settings
 *
 *   value column (BGM: track list: SFX: set list - PSP/MGS/PS4)
 *     -> Up/Down moves AND live-previews immediately (same instant-preview
 *        feel the old single-level BGM picker had)
 *     -> X confirms and slides back to the category column
 *     -> Circle backs out to the category column without reverting
 *        (whatever was last highlighted while browsing is what stays
 *        selected - same behaviour the theme submenu already has)
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "common/sio0.h"
#include "main/defs.h"
#include "main/font.h"
#include "main/music_settings.h"
#include "main/renderer.h"
#include "main/sound.h"
#include "main/xmb_bg.h"

enum { CAT_BGM = 0, CAT_SFX = 1, CAT_COUNT = 2 };
static const char *const categoryNames[CAT_COUNT] = { "BGM", "SFX" };

static int valueCount(int cat) {
	return (cat == CAT_BGM) ? getBGMCount() : getSFXSetCount();
}
static const char *valueName(int cat, int i) {
	return (cat == CAT_BGM) ? getBGMName(i) : getSFXSetName(i);
}
static int valueCurrent(int cat) {
	return (cat == CAT_BGM) ? getBGMIndex() : getSFXSetIndex();
}
static void valueApply(int cat, int i) {
	if (cat == CAT_BGM)
		selectBGM(i);
	else
		selectSFXSet(i);
}

void runMusicSettings(RenderContext *ctx, UIState *state, const MenuItem *item) {
	(void) state;
	(void) item;

	// Debounce the button that opened this screen.
	while (pollController(0) | pollController(1))
		;

	int catIndex = CAT_BGM;
	// Start on the first value rather than the applied one: the list marks
	// what is currently applied anyway, and opening from a fixed position
	// makes the screen behave the same way every time.
	int valIndex = 0;
	bool valueOpen = false;
	int slideFx = 0;   // 0 = category column only, 256 = value column fully in

	uint16_t lastPad = 0;

	for (;;) {
		uint16_t pad     = pollController(0) | pollController(1);
		uint16_t pressed = pad & ~lastPad;
		lastPad          = pad;

		if (valueOpen) {
			int count = valueCount(catIndex);

			if ((pressed & PAD_BTN_UP) && valIndex > 0) {
				valIndex--;
				playScrollSound();
				valueApply(catIndex, valIndex);   // instant preview
			}
			if ((pressed & PAD_BTN_DOWN) && valIndex < count - 1) {
				valIndex++;
				playScrollSound();
				valueApply(catIndex, valIndex);
			}
			if (pressed & PAD_BTN_CROSS) {
				playConfirmSound();
				valueOpen = false;
			}
			if (pressed & PAD_BTN_CIRCLE) {
				playCancelSound();
				valueOpen = false;   // back to category column, keep the preview
			}
		} else {
			if ((pressed & PAD_BTN_UP) && catIndex > 0) {
				catIndex--;
				playScrollSound();
			}
			if ((pressed & PAD_BTN_DOWN) && catIndex < CAT_COUNT - 1) {
				catIndex++;
				playScrollSound();
			}
			if (pressed & PAD_BTN_CROSS) {
				playConfirmSound();
				valIndex  = 0;   // open at the top, see above
				valueOpen = true;
			}
			if (pressed & PAD_BTN_CIRCLE) {
				playCancelSound();
				break;   // exit the whole screen
			}
		}

		// Simple slide: value column eases toward 256 (fully open) or 0
		// (closed) each frame - the same style of animation the theme
		// submenu uses for its own value column, just driven by a plain
		// linear step here rather than that system's own state machine.
		int target = valueOpen ? 256 : 0;
		if (slideFx < target) { slideFx += 48; if (slideFx > target) slideFx = target; }
		if (slideFx > target) { slideFx -= 48; if (slideFx < target) slideFx = target; }

		beginFrame(ctx);
		drawXMBBackground(ctx);

		printString(ctx, 16, 20, 0xffffff, "Music");

		// --- category column ---
		int catX = 16 - (slideFx * 40 >> 8);   // eases left as the value column opens
		for (int i = 0; i < CAT_COUNT; i++) {
			bool isSel = (i == catIndex) && !valueOpen;
			char line[16];
			snprintf(line, sizeof(line), "%s %s",
				(i == catIndex) ? ">" : " ", categoryNames[i]);
			printString(ctx, catX, 60 + i * 16, isSel ? 0xffffff : 0x808080, line);
		}

		// --- value column: slides in from the right ---
		if (slideFx > 0) {
			int count  = valueCount(catIndex);
			int valX   = 140 + ((256 - slideFx) * 60 >> 8);
			int applied = valueCurrent(catIndex);

			char header[24];
			snprintf(header, sizeof(header), "-- %s --", categoryNames[catIndex]);
			printString(ctx, valX, 44, 0x808080, header);

			int y = 60;
			for (int i = 0; i < count; i++) {
				bool isSel = (i == valIndex) && valueOpen;
				char line[40];
				snprintf(line, sizeof(line), "%s %s%s",
					isSel ? ">" : " ", valueName(catIndex, i),
					(i == applied) ? " *" : "");
				// Greyed out if this build does not contain the track.
				bool avail = (catIndex != CAT_BGM) || bgmTrackAvailable(i);
				printString(ctx, valX, y,
					!avail  ? 0x303030 :
					isSel   ? 0xffffff : 0x808080, line);
				y += 16;
			}
		}

		if (valueOpen)
			printString(ctx, 16, ctx->screenHeight - 26, 0x606060,
				CH_PS1_DPAD_Y " Select   " CH_PS1_CROSS_BUTTON " Confirm   "
				CH_PS1_CIRCLE_BUTTON " Back");
		else
			printString(ctx, 16, ctx->screenHeight - 26, 0x606060,
				CH_PS1_DPAD_Y " Select   " CH_PS1_CROSS_BUTTON " Open   "
				CH_PS1_CIRCLE_BUTTON " Back");

		endFrame(ctx);
	}

	// Debounce on the way out so the outer XMB doesn't re-read the press.
	while (pollController(0) | pollController(1))
		;
}
