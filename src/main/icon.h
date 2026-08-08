/*
 * PSX-iTests - menu icon sheets
 *
 * Two 4bpp icon sheets are uploaded to VRAM: one for the top-level category
 * icons and one for the submenu item icons. Both use 64x64 source cells and
 * are drawn scaled to whatever size the menu needs.
 *
 * They are split because a PS1 texture page tops out at 256x256 texels (UV
 * coordinates are 8-bit), so all 17 icons could not share one sheet at 64x64
 * without overflowing that limit - see icon.c for the full layout notes.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "main/renderer.h"

// Category sheet order must match the categories[] array in xmb_menu.c:
// Settings, Themes, Music, Game, Memory Card Manager, Hardware Tester.
#define ICON_CAT_COUNT   6
#define ICON_ITEM_COUNT 12

#ifdef __cplusplus
extern "C" {
#endif

// Upload both icon sheets to VRAM. Call once after setupRenderer().
void initIcons(RenderContext *ctx);

// Draw submenu item icon `index` (0..ICON_ITEM_COUNT-1) with its top-left at
// (x, y), scaled to size x size pixels. translucent draws it with the PS1's
// ~50% semi-transparency blend instead of fully opaque.
void drawIcon(
	RenderContext *ctx, int index, int x, int y, int size, bool translucent
);

// Same, for a top-level category icon (0..ICON_CAT_COUNT-1).
void drawCategoryIcon(
	RenderContext *ctx, int index, int x, int y, int size, bool translucent
);

// As drawCategoryIcon(), but lays a vertical colour gradient over the icon
// (POLY_GT4): cTop tints the top edge, cBot the bottom edge, with the GPU
// interpolating between them. Colours are raw gp0_rgb() modulation values
// (0x808080 == neutral). Used by the themed XMB row so the row icons pick up
// the active theme's colours without any extra artwork.
void drawCategoryIconGradient(
	RenderContext *ctx, int index, int x, int y, int size, bool translucent,
	uint32_t cTop, uint32_t cBot
);


/*
 * Pad tester button glyphs (see drawPadGlyph in icon.c).
 *
 * One sheet holds both states: cells 0..14 unpressed, +PAD_GLYPH_PRESSED for
 * the pressed version of the same button. The pressed state is a different
 * piece of artwork rather than a colour or a highlight box behind the glyph.
 */
#define PAD_GLYPH_W       18
#define PAD_GLYPH_H       16
#define PAD_GLYPH_COLS     8
#define PAD_GLYPH_CELLS   32
#define PAD_GLYPH_PRESSED 16

typedef enum {
	PAD_GLYPH_CIRCLE = 0,
	PAD_GLYPH_CROSS,
	PAD_GLYPH_TRIANGLE,
	PAD_GLYPH_SQUARE,
	PAD_GLYPH_SELECT,
	PAD_GLYPH_START,
	PAD_GLYPH_L1,
	PAD_GLYPH_R1,
	PAD_GLYPH_L2,
	PAD_GLYPH_R2,
	PAD_GLYPH_STICK,
	PAD_GLYPH_DPAD_UP,
	PAD_GLYPH_DPAD_DOWN,
	PAD_GLYPH_DPAD_LEFT,
	PAD_GLYPH_DPAD_RIGHT,
	/* Standalone 19x19 dashboard navigation D-pad appended after the grid. */
	PAD_GLYPH_NAVIGATE
} PadGlyph;

void drawPadGlyph(RenderContext *ctx, int index, int x, int y);

/* Draw a pad glyph at an arbitrary size/tint. Used by the shared font
 * renderer so inline prompts use this exact same artwork as Pad Tester. */
void drawPadGlyphSized(
	RenderContext *ctx, int index, int x, int y, int width, int height,
	uint32_t color
);

#ifdef __cplusplus
}
#endif
