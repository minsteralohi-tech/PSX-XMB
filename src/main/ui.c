/*
 * ps1-ram-tester - (C) 2026 spicyjpeg
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
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "common/sio0.h"
#include "main/defs.h"
#include "main/font.h"
#include "main/ramconfig.h"
#include "main/ramtester.h"
#include "main/renderer.h"
#include "main/sound.h"
#include "main/ui.h"
#include "main/xmb_bg.h"

#define MARGIN_LEFT   16
#define MARGIN_RIGHT  16
#define MARGIN_TOP    20
#define MARGIN_BOTTOM  8

#define ITEM_HEIGHT        (FONT_LINE_HEIGHT + 4)
#define STATIC_ITEM_HEIGHT (FONT_LINE_HEIGHT + 1)
#define SEPARATOR_HEIGHT   (ITEM_HEIGHT / 2)
#define HIGHLIGHT_PADDING  2

#define PROGRESS_BAR_HEIGHT  10
#define PROGRESS_BAR_SPACING  8

#define BUTTON_REPEAT_DELAY    30
#define BUTTON_REPEAT_INTERVAL 10

enum Color {
	COLOR_WINDOW1     = 0x505050,
	COLOR_WINDOW2     = 0x242424,
	COLOR_WINDOW3     = 0x080808,
	COLOR_HIGHLIGHT1  = 0x1256e3,
	COLOR_HIGHLIGHT2  = 0x032e9c,
	COLOR_PROGRESS1   = 0x10c048,
	COLOR_PROGRESS2   = 0x007820,
	COLOR_TEXT1       = 0x808080,
	COLOR_TEXT2       = 0x505050,
	COLOR_TEXT3       = 0x383838,
	COLOR_TEXT_ACTIVE = 0x4078a0
};

/* Utilities */

static void moveMenuCursor(UIState *state, int step) {
	int             newCursor = state->menuCursor;
	const MenuItem *newItem   = &state->currentMenu[newCursor];

	// This is very crude and will break if step is not 1 or -1.
	do {
		newCursor += step;
		newItem   += step;

		if (newCursor < 0)
			return; // clamp at the top, no wrap-around
		if (newItem->type == ITEM_END)
			return; // clamp at the bottom, no wrap-around
	} while (newItem->type < ITEM_ACTION);

	state->menuCursor = newCursor;
	playScrollSound();
}

static void drawButtonPrompt(RenderContext *ctx, const char *prompt) {
	const char *version = "psx-itests-v" VERSION_STRING;
	int        promptY  =
		ctx->screenHeight - (MARGIN_BOTTOM + FONT_LINE_HEIGHT);

	// Native size, not scaled down - an earlier attempt at a smaller footer
	// used printStringScaled() here, but shrinking this tiny bitmap font by
	// a non-integer factor looked genuinely bad on a real CRT (fractional
	// scaling of small pixel art is a known problem - it comes out blurry/
	// illegible rather than cleanly smaller), confirmed on real hardware.
	// Reverted; printStringScaled() itself is left in font.c in case a
	// future change wants it for something where that tradeoff works
	// better (larger source text, or a deliberate 2x/3x integer scale).
	printString(ctx, MARGIN_LEFT, promptY, COLOR_TEXT3, prompt);
	printString(
		ctx,
		ctx->screenWidth - (MARGIN_RIGHT + getStringWidth(version)),
		promptY,
		COLOR_TEXT3,
		version
	);
}

void updateUIState(UIState *state, uint16_t buttons) {
	uint16_t changed        = buttons ^ state->lastButtons;
	state->buttonsPressed   = buttons & changed;
	state->buttonsRepeating = buttons & changed;

	if (buttons && !changed) {
		state->repeatTimer++;

		if (state->repeatTimer >= BUTTON_REPEAT_DELAY) {
			int sinceDelay = state->repeatTimer - BUTTON_REPEAT_DELAY;
			if ((sinceDelay % BUTTON_REPEAT_INTERVAL) == 0)
				state->buttonsRepeating |= buttons;
		}
	} else {
		state->repeatTimer = 0;
	}

	state->lastButtons = buttons;
}

void setupUIState(UIState *state) {
	state->currentMenu      = 0;
	state->buttons          = 0;
	state->lastButtons      = 0;
	state->buttonsPressed   = 0;
	state->buttonsRepeating = 0;
	state->repeatTimer      = 0;
	state->menuCursor       = 0;
}

int findFirstSelectableItem(const MenuItem *menu) {
	int index = 0;

	while ((menu[index].type != ITEM_END) && (menu[index].type < ITEM_ACTION))
		index++;

	return (menu[index].type == ITEM_END) ? 0 : index;
}

/* Menus */

static const char *menuButtonPrompts[] = {
	[ITEM_ACTION] =
		CH_PS1_DPAD " Navigate   "
		CH_PS1_CROSS_BUTTON " Select   " CH_PS1_CIRCLE_BUTTON " Back",
	[ITEM_INT]    = CH_PS1_DPAD " Navigate / Adjust",
	[ITEM_BINARY] = CH_PS1_DPAD " Navigate / Adjust",
	[ITEM_ENUM]   = CH_PS1_DPAD " Navigate / Adjust"
};

// True for the RAM tester and RAM config screens - the only screens in the
// app that read/write live hardware timing/addressing registers (DRAM_CTRL,
// GP1 VRAM size) rather than just app state. Their own progress-bar screen
// (renderProgressScreen(), shown while a test is actually running) has its
// own identical fix now too - it turned out to still be drawing the full
// live theme this whole time, on every single progress update, despite an
// earlier assumption that it already skipped it. This covers the menus
// around the tests (including "Configure main RAM", which can sit there
// with a live DRAM_CTRL value applied while the user browses other
// options). A themed background scales its own primitive count and CPU
// cost with whichever theme happens to be selected - fine for ordinary
// menus, but not something that should be stacked on top of screens
// already poking at memory timing/addressing hardware directly.
static bool isHeavyBackgroundUnsafe(const MenuItem *menu) {
	return isRAMTesterMenu(menu) || isRAMConfigMenu(menu);
}

void renderMenu(RenderContext *ctx, const UIState *state) {
	if (isHeavyBackgroundUnsafe(state->currentMenu))
		drawFlatBackdrop(ctx, gp0_rgb(0, 0, 0));
	else
		drawXMBBackground(ctx);

	const MenuItem *item = state->currentMenu;

	int cursorOffset = state->menuCursor;
	int currentY     = MARGIN_TOP;

	while (item->type != ITEM_END) {
		// Determine the item's state.
		uint32_t color  = COLOR_TEXT1;
		int      height = ITEM_HEIGHT;

		const char *value = 0;
		char       buffer[8];

		switch (item->type) {
			case ITEM_SEPARATOR:
				height = SEPARATOR_HEIGHT;
				goto _nextItem;

			case ITEM_TITLE:
				height = STATIC_ITEM_HEIGHT;
				break;

			case ITEM_STATIC:
				color  = COLOR_TEXT2;
				height = STATIC_ITEM_HEIGHT;
				break;

			case ITEM_ACTION:
				if (item->action.tag)
					value = item->action.tag;
				break;

			case ITEM_INT:
				snprintf(buffer, sizeof(buffer), "%d", *item->int_.value);
				value = buffer;
				break;

			case ITEM_BINARY:
				snprintf(
					buffer,
					sizeof(buffer),
					"%0*b",
					item->bitLength,
					*item->int_.value
				);
				value = buffer;
				break;

			case ITEM_ENUM:
				value = item->enum_.items[*item->enum_.value];
				break;

			default:
				assert(false);
		}

		// Draw the item.
		int valueWidth = getStringWidth(value);

		if (!cursorOffset) {
			drawRect(
				ctx,
				MARGIN_LEFT - HIGHLIGHT_PADDING,
				currentY - HIGHLIGHT_PADDING,
				ctx->screenWidth
					- (MARGIN_LEFT + MARGIN_RIGHT - HIGHLIGHT_PADDING * 2),
				height,
				COLOR_HIGHLIGHT1,
				false
			);

			if (valueWidth)
				drawRect(
					ctx,
					ctx->screenWidth
						- (MARGIN_RIGHT + HIGHLIGHT_PADDING * 3 + valueWidth),
					currentY - HIGHLIGHT_PADDING,
					valueWidth + HIGHLIGHT_PADDING * 4,
					height,
					COLOR_HIGHLIGHT2,
					false
				);
		}

		printString(ctx, MARGIN_LEFT, currentY, color, item->name);
		printString(
			ctx,
			ctx->screenWidth - (MARGIN_RIGHT + valueWidth),
			currentY,
			cursorOffset ? COLOR_TEXT2 : COLOR_TEXT1,
			value
		);

_nextItem:
		item++;
		cursorOffset--;
		currentY += height;
	}

	item = &state->currentMenu[state->menuCursor];

	drawButtonPrompt(ctx, menuButtonPrompts[item->type]);
}

void updateMenu(RenderContext *ctx, UIState *state, uint16_t buttons) {
	updateUIState(state, buttons);

	uint16_t pressed   = state->buttonsPressed;
	uint16_t repeating = state->buttonsRepeating;

	// Handle the currently highlighted item.
	const MenuItem *item = &state->currentMenu[state->menuCursor];
	int            value;

	switch (item->type) {
		case ITEM_ACTION:
			if (pressed & (PAD_BTN_START | PAD_BTN_CIRCLE | PAD_BTN_CROSS)) {
				playConfirmSound();
				item->action.callback(ctx, state, item);
			}
			break;

		case ITEM_INT:
		case ITEM_BINARY:
		case ITEM_ENUM:
			// Note that this assumes item->int_.value aliases to
			// item->enum_.value.
			value = *item->int_.value;

			if (value > item->minValue) {
				if (repeating & PAD_BTN_LEFT)
					*item->int_.value = value - 1;
			} else {
				if (pressed & PAD_BTN_LEFT)
					*item->int_.value = item->maxValue;
			}

			if (value < item->maxValue) {
				if (repeating & PAD_BTN_RIGHT)
					*item->int_.value = value + 1;
			} else {
				if (pressed & PAD_BTN_RIGHT)
					*item->int_.value = item->minValue;
			}
			break;

		default:
			assert(false);
	}

	// Handle menu navigation.
	uint16_t upMask   = !state->menuCursor         ? pressed : repeating;
	uint16_t downMask = (item[1].type == ITEM_END) ? pressed : repeating;

	if (upMask   & PAD_BTN_UP)
		moveMenuCursor(state, -1);
	if (downMask & PAD_BTN_DOWN)
		moveMenuCursor(state,  1);
}

/* Progress bar screen */

void renderProgressScreen(
	RenderContext *ctx,
	int           progress,
	int           total,
	const char    *message
) {
	// Always the flat backdrop, never the live theme - this screen is used
	// by the RAM/VRAM/SPU RAM tests, the CPU benchmark, and the reboot
	// warning. All three actively want this: the RAM tests are exactly
	// the "heavy theme selected -> crash" case this whole file's
	// isHeavyBackgroundUnsafe() exists for (an earlier assumption that
	// this screen already skipped the theme was wrong - it didn't, it's
	// called on every single progress update during the actual test, so
	// the theme was re-rendering the whole time the test ran, not just
	// while browsing the surrounding menus); the CPU benchmark times
	// itself against real vblank periods, so a heavy theme competing for
	// the same frame's GPU/CPU time would actually skew its result, not
	// just cost performance; and there's no reason to animate a theme
	// while the console is about to reboot.
	drawFlatBackdrop(ctx, gp0_rgb(0, 0, 0));

	int textY      = (ctx->screenHeight - PROGRESS_BAR_SPACING) / 2;
	int barY       = (ctx->screenHeight + PROGRESS_BAR_SPACING) / 2;
	int totalWidth = ctx->screenWidth - (MARGIN_LEFT + MARGIN_RIGHT);

	printString(
		ctx,
		MARGIN_LEFT,
		textY - FONT_LINE_HEIGHT,
		COLOR_TEXT1,
		message
	);
	drawRect(
		ctx,
		MARGIN_LEFT,
		barY,
		totalWidth,
		PROGRESS_BAR_HEIGHT,
		COLOR_WINDOW3,
		true
	);
	drawGradientRectH(
		ctx,
		MARGIN_LEFT,
		barY,
		(totalWidth * progress) / total,
		PROGRESS_BAR_HEIGHT,
		COLOR_PROGRESS2,
		COLOR_PROGRESS1,
		false
	);

	drawButtonPrompt(ctx, 0);
}
