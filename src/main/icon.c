/*
 * PSX-iTests - menu icon sheets (see icon.h)
 */

#include <stdint.h>
#include <stdbool.h>
#include "common/gpu.h"
#include "main/icon.h"
#include "main/renderer.h"
#include "ps1/gpucmd.h"

// Sheets + palettes, embedded via addBinaryFile() in CMakeLists.txt.
extern const uint8_t catTexture[],  catPalette[];
extern const uint8_t itemTexture[], itemPalette[];

#define ICON_CELL  64   // px per icon cell in both sheets
#define ICON_COLS  4    // cells per row in both sheets

#define CAT_SHEET_W   256
#define CAT_SHEET_H   128   // 4 x 2 cells = 6 category icons (2 spare)
#define ITEM_SHEET_W  256
#define ITEM_SHEET_H  192   // 4 x 3 cells = 11 item icons (1 spare)

/*
 * Why two sheets instead of one:
 *
 * A PS1 texture page is at most 256x256 texels, because UV coordinates are
 * only 8 bits and cannot address past the end of a page. All 17 icons at
 * 64x64 in a single sheet would need 4 columns x 5 rows = 256x320, which
 * overflows that limit and would render garbage for the last row. Splitting
 * categories and items into two sheets keeps both comfortably inside one
 * page each while preserving the full 64x64 source resolution.
 *
 * VRAM placement (x is in VRAM halfword units). The two 320-wide
 * framebuffers occupy x 0..640, and the existing bg/font block sits at
 * x 640..672, y 0..113. A 256-texel-wide 4bpp sheet is 64 halfwords, and
 * must start on a multiple of 64 for its U coordinate to come out as 0:
 *
 *   categories : x 704..768, y 0..128
 *   items      : x 768..832, y 0..192
 *   CLUTs      : x 704 and 720, y 256   (16-aligned, clear of both sheets)
 */
#define CAT_VRAM_X    704
#define CAT_VRAM_Y      0
#define CAT_CLUT_X    704
#define CAT_CLUT_Y    256

#define ITEM_VRAM_X   768
#define ITEM_VRAM_Y     0
#define ITEM_CLUT_X   720
#define ITEM_CLUT_Y   256

static TextureInfo catTex;
static TextureInfo itemTex;

void initIcons(RenderContext *ctx) {
	(void) ctx;

	uploadIndexedTexture(
		&catTex, catTexture, catPalette,
		CAT_VRAM_X, CAT_VRAM_Y, CAT_CLUT_X, CAT_CLUT_Y,
		CAT_SHEET_W, CAT_SHEET_H, GP0_COLOR_4BPP
	);
	uploadIndexedTexture(
		&itemTex, itemTexture, itemPalette,
		ITEM_VRAM_X, ITEM_VRAM_Y, ITEM_CLUT_X, ITEM_CLUT_Y,
		ITEM_SHEET_W, ITEM_SHEET_H, GP0_COLOR_4BPP
	);
}

/*
 * Textured quad (POLY_FT4) mapping one icon cell to an arbitrary rectangle.
 * color modulates the texture: 0x808080 == neutral (draw as-is). Texels whose
 * CLUT index is 0 are transparent, so the icon background drops out.
 *
 * When blend is set the quad uses the texpage's blend mode, which
 * uploadIndexedTexture() already configured as SEMITRANS - the PS1's ~50%
 * see-through mode. Opaque draws ignore it.
 */
static void iconQuad(
	RenderContext *ctx, const TextureInfo *tex, int index,
	int x, int y, int w, int h, uint32_t color, bool blend
) {
	GPUDMAChain *chain = getCurrentChain(ctx);

	int cx = (index % ICON_COLS) * ICON_CELL;
	int cy = (index / ICON_COLS) * ICON_CELL;
	int u0 = tex->u + cx,        v0 = tex->v + cy;
	int u1 = u0 + ICON_CELL - 1, v1 = v0 + ICON_CELL - 1;

	// Re-select this sheet's texture page each draw, since other menu
	// elements (the other sheet, text) change it in between.
	uint32_t *page = allocateGP0Packet(chain, 1);
	page[0] = gp0_setPage(tex->page, false, false);

	uint32_t *ptr = allocateGP0Packet(chain, 9);
	ptr[0] = color | gp0_shadedQuad(false, true, blend);
	ptr[1] = gp0_xy(x,     y);
	ptr[2] = gp0_uv(u0, v0, tex->clut);
	ptr[3] = gp0_xy(x + w, y);
	ptr[4] = gp0_uv(u1, v0, tex->page);
	ptr[5] = gp0_xy(x,     y + h);
	ptr[6] = gp0_uv(u0, v1, 0);
	ptr[7] = gp0_xy(x + w, y + h);
	ptr[8] = gp0_uv(u1, v1, 0);
}

/*
 * Gouraud + textured quad (POLY_GT4). Same as iconQuad(), but instead of one
 * flat modulation colour it gives each of the four corners its own colour, so
 * the texture is modulated by a smooth gradient interpolated across the icon.
 * This is how we lay a colour gradient over an icon without uploading any new
 * artwork - the GPU interpolates the vertex colours for free.
 *
 * Modulation maths: out = texel * colour / 128, so 0x808080 is neutral (1.0x)
 * and a corner colour of (r,g,b) tints a white icon texel toward roughly
 * (2r, 2g, 2b) (clamped). cTop is used for the two top corners, cBot for the
 * two bottom corners -> a vertical top-to-bottom gradient.
 */
static void iconQuadGradient(
	RenderContext *ctx, const TextureInfo *tex, int index,
	int x, int y, int w, int h, uint32_t cTop, uint32_t cBot, bool blend
) {
	GPUDMAChain *chain = getCurrentChain(ctx);

	int cx = (index % ICON_COLS) * ICON_CELL;
	int cy = (index / ICON_COLS) * ICON_CELL;
	int u0 = tex->u + cx,        v0 = tex->v + cy;
	int u1 = u0 + ICON_CELL - 1, v1 = v0 + ICON_CELL - 1;

	uint32_t *page = allocateGP0Packet(chain, 1);
	page[0] = gp0_setPage(tex->page, false, false);

	// Gouraud + textured quad: 12 words, colour interleaved before each vertex.
	uint32_t *ptr = allocateGP0Packet(chain, 12);
	ptr[0]  = cTop | gp0_shadedQuad(true, true, blend);
	ptr[1]  = gp0_xy(x,     y);
	ptr[2]  = gp0_uv(u0, v0, tex->clut);
	ptr[3]  = cTop;
	ptr[4]  = gp0_xy(x + w, y);
	ptr[5]  = gp0_uv(u1, v0, tex->page);
	ptr[6]  = cBot;
	ptr[7]  = gp0_xy(x,     y + h);
	ptr[8]  = gp0_uv(u0, v1, 0);
	ptr[9]  = cBot;
	ptr[10] = gp0_xy(x + w, y + h);
	ptr[11] = gp0_uv(u1, v1, 0);
}

void drawIcon(
	RenderContext *ctx, int index, int x, int y, int size, bool translucent
) {
	if (index < 0 || index >= ICON_ITEM_COUNT)
		return;

	iconQuad(ctx, &itemTex, index, x, y, size, size, 0x808080, translucent);
}

void drawCategoryIconGradient(
	RenderContext *ctx, int index, int x, int y, int size, bool translucent,
	uint32_t cTop, uint32_t cBot
) {
	if (index < 0 || index >= ICON_CAT_COUNT)
		return;

	iconQuadGradient(ctx, &catTex, index, x, y, size, size,
		cTop, cBot, translucent);
}

void drawCategoryIcon(
	RenderContext *ctx, int index, int x, int y, int size, bool translucent
) {
	if (index < 0 || index >= ICON_CAT_COUNT)
		return;

	iconQuad(ctx, &catTex, index, x, y, size, size, 0x808080, translucent);
}


/* --- translucent "crystal" panel ---------------------------------------- */

static uint32_t glassScale(uint32_t colour, int numerator, int denominator) {
	uint32_t r = ((colour        & 0xff) * numerator) / denominator;
	uint32_t g = (((colour >> 8)  & 0xff) * numerator) / denominator;
	uint32_t b = (((colour >> 16) & 0xff) * numerator) / denominator;

	if (r > 0xff) r = 0xff;
	if (g > 0xff) g = 0xff;
	if (b > 0xff) b = 0xff;

	return (b << 16) | (g << 8) | r;
}

/*
 * XMB-style crystal tile - see renderer.h.
 *
 * The GPU has no alpha channel, no rounded primitives and no blur, so the
 * glassy look is assembled from flat quads:
 *
 *   - the body is drawn with blend=true, which uses the semi-transparent
 *     blend mode, so the wallpaper shows through and the tile reads as glass
 *     rather than a painted block;
 *   - a lighter band across the top is the specular sheen. Two bands of
 *     slightly different brightness approximate a vertical gradient, which
 *     the renderer cannot do directly (drawGradientRectH is horizontal only);
 *   - a bright 1px edge along the top and left plus a dark one along the
 *     bottom and right is a standard bevel, and is what gives the 3D lift;
 *   - the top and bottom rows are inset by one pixel, which reads as a
 *     rounded corner at these sizes for the cost of two extra quads.
 *
 * `tint` wants to be DARK. Blending adds the background on top and the sheen
 * and bevel lighten it further, so a mid-brightness tint comes out washed
 * rather than glassy. xmbGetAccentColor() already returns a suitable value.
 */
void drawGlassPanel(
	RenderContext *ctx, int x, int y, int w, int h,
	uint32_t tint, uint32_t glow
) {
	if (w <= 2 || h <= 2)
		return;

	if (glow) {
		/* Outward bloom: larger and dimmer with each ring, blended so they
		 * accumulate instead of replacing what is underneath. */
		drawRect(ctx, x - 4, y - 4, w + 8, h + 8, glassScale(glow, 1, 4), true);
		drawRect(ctx, x - 3, y - 3, w + 6, h + 6, glassScale(glow, 1, 2), true);
		drawRect(ctx, x - 2, y - 2, w + 4, h + 4, glow, true);
	}

	/* Body, with the top and bottom rows inset to round the corners. */
	drawRect(ctx, x,     y + 1,     w,     h - 2, tint, true);
	drawRect(ctx, x + 1, y,         w - 2, 1,     tint, true);
	drawRect(ctx, x + 1, y + h - 1, w - 2, 1,     tint, true);

	/* Specular sheen across the top, two steps for a soft falloff. */
	drawRect(ctx, x + 1, y + 1,         w - 2, h / 3, glassScale(tint, 9, 4), true);
	drawRect(ctx, x + 1, y + 1 + h / 3, w - 2, h / 6, glassScale(tint, 6, 4), true);

	/* Bevel: light from the top left, shadow to the bottom right. */
	uint32_t lit   = glassScale(tint, 5, 2);
	uint32_t shade = glassScale(tint, 1, 3);

	drawRect(ctx, x + 1,     y,         w - 2, 1,     lit,   true);
	drawRect(ctx, x,         y + 1,     1,     h - 2, lit,   true);
	drawRect(ctx, x + 1,     y + h - 1, w - 2, 1,     shade, true);
	drawRect(ctx, x + w - 1, y + 1,     1,     h - 2, shade, true);
}
