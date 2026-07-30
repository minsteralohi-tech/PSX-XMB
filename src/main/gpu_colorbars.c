/*
 * PSX-iTests - GPU color bar test pattern
 *
 * A classic-style test pattern: vertical color bars filling the top of the
 * screen plus a black-to-white gradient strip at the bottom, useful for a
 * quick visual sanity check of color output, contrast and screen geometry.
 * Exits back to the main menu on any button press.
 */

#include <stdint.h>
#include "common/sio0.h"
#include "main/gpu_colorbars.h"
#include "main/mainmenu.h"

#define NUM_BARS 7

static const uint32_t barColors[NUM_BARS] = {
	0xc0c0c0, // white (75%, avoids clipping on displays that crush pure white)
	0xc0c000, // yellow
	0x00c0c0, // cyan
	0x00c000, // green
	0xc000c0, // magenta
	0xc00000, // red
	0x0000c0  // blue
};

void runColorBarTest(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
) {
	(void) state;
	(void) item;

	int barWidth    = ctx->screenWidth / NUM_BARS;
	int usedWidth   = barWidth * NUM_BARS;
	int barsHeight  = (ctx->screenHeight * 2) / 3;
	int gradientY   = barsHeight;
	int gradientH   = ctx->screenHeight - barsHeight;

	// Debounce: wait for the button that opened this screen to be released,
	// then wait for a fresh press, so it doesn't instantly exit again.
	while (pollController(0) | pollController(1))
		;

	for (;;) {
		beginFrame(ctx);

		for (int i = 0; i < NUM_BARS; i++)
			drawRect(
				ctx, i * barWidth, 0, barWidth, barsHeight, barColors[i], false
			);

		// Cover any leftover width left over from integer division.
		if (usedWidth < ctx->screenWidth)
			drawRect(
				ctx,
				usedWidth,
				0,
				ctx->screenWidth - usedWidth,
				barsHeight,
				barColors[NUM_BARS - 1],
				false
			);

		drawGradientRectH(
			ctx, 0, gradientY, ctx->screenWidth, gradientH,
			0x000000, 0xffffff, false
		);

		endFrame(ctx);

		if (pollController(0) | pollController(1))
			break;
	}

	// Flush button state before handing control back to the outer menu
	// system - see the matching comment in memcard.c's
	// runMemoryCardManager() for the full explanation. Without this,
	// whichever button you exited with is still held when control
	// returns, and the outer menu's stale lastButtons misreads it as a
	// fresh "confirm" press on this same still-highlighted item,
	// immediately re-opening this screen.
	while (pollController(0) | pollController(1))
		;

	// Deliberately no enterMainMenu() call here - state->currentMenu and
	// state->menuCursor were never touched anywhere in this function
	// (this screen renders through its own self-contained loop
	// instead), so they still correctly point at whichever item was
	// selected to get here.
}
