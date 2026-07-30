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
#include "main/renderer.h"

// Category sheet order must match the categories[] array in xmb_menu.c:
// Settings, Themes, Music, Game, Memory Card Manager, Hardware Tester.
#define ICON_CAT_COUNT   6
#define ICON_ITEM_COUNT 11

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

#ifdef __cplusplus
}
#endif
