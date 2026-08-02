/*
 * ps1-bare-metal - (C) 2023-2025 spicyjpeg
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

#include <stdint.h>
#include "common/gpu.h"
#include "main/font.h"
#include "main/renderer.h"
#include "ps1/gpucmd.h"

typedef struct {
	uint8_t x, y, width, height;
} SpriteInfo;

static const SpriteInfo fontSprites[] = {
	{ .x =  6, .y =  0, .width = 2, .height = 9 }, // !
	{ .x = 12, .y =  0, .width = 4, .height = 9 }, // "
	{ .x = 18, .y =  0, .width = 6, .height = 9 }, // #
	{ .x = 24, .y =  0, .width = 6, .height = 9 }, // $
	{ .x = 30, .y =  0, .width = 6, .height = 9 }, // %
	{ .x = 36, .y =  0, .width = 6, .height = 9 }, // &
	{ .x = 42, .y =  0, .width = 2, .height = 9 }, // '
	{ .x = 48, .y =  0, .width = 3, .height = 9 }, // (
	{ .x = 54, .y =  0, .width = 3, .height = 9 }, // )
	{ .x = 60, .y =  0, .width = 4, .height = 9 }, // *
	{ .x = 66, .y =  0, .width = 6, .height = 9 }, // +
	{ .x = 72, .y =  0, .width = 3, .height = 9 }, // ,
	{ .x = 78, .y =  0, .width = 6, .height = 9 }, // -
	{ .x = 84, .y =  0, .width = 2, .height = 9 }, // .
	{ .x = 90, .y =  0, .width = 6, .height = 9 }, // /
	{ .x =  0, .y =  9, .width = 6, .height = 9 }, // 0
	{ .x =  6, .y =  9, .width = 6, .height = 9 }, // 1
	{ .x = 12, .y =  9, .width = 6, .height = 9 }, // 2
	{ .x = 18, .y =  9, .width = 6, .height = 9 }, // 3
	{ .x = 24, .y =  9, .width = 6, .height = 9 }, // 4
	{ .x = 30, .y =  9, .width = 6, .height = 9 }, // 5
	{ .x = 36, .y =  9, .width = 6, .height = 9 }, // 6
	{ .x = 42, .y =  9, .width = 6, .height = 9 }, // 7
	{ .x = 48, .y =  9, .width = 6, .height = 9 }, // 8
	{ .x = 54, .y =  9, .width = 6, .height = 9 }, // 9
	{ .x = 60, .y =  9, .width = 2, .height = 9 }, // :
	{ .x = 66, .y =  9, .width = 3, .height = 9 }, // ;
	{ .x = 72, .y =  9, .width = 6, .height = 9 }, // <
	{ .x = 78, .y =  9, .width = 6, .height = 9 }, // =
	{ .x = 84, .y =  9, .width = 6, .height = 9 }, // >
	{ .x = 90, .y =  9, .width = 6, .height = 9 }, // ?
	{ .x =  0, .y = 18, .width = 6, .height = 9 }, // @
	{ .x =  6, .y = 18, .width = 6, .height = 9 }, // A
	{ .x = 12, .y = 18, .width = 6, .height = 9 }, // B
	{ .x = 18, .y = 18, .width = 6, .height = 9 }, // C
	{ .x = 24, .y = 18, .width = 6, .height = 9 }, // D
	{ .x = 30, .y = 18, .width = 6, .height = 9 }, // E
	{ .x = 36, .y = 18, .width = 6, .height = 9 }, // F
	{ .x = 42, .y = 18, .width = 6, .height = 9 }, // G
	{ .x = 48, .y = 18, .width = 6, .height = 9 }, // H
	{ .x = 54, .y = 18, .width = 4, .height = 9 }, // I
	{ .x = 60, .y = 18, .width = 5, .height = 9 }, // J
	{ .x = 66, .y = 18, .width = 6, .height = 9 }, // K
	{ .x = 72, .y = 18, .width = 6, .height = 9 }, // L
	{ .x = 78, .y = 18, .width = 6, .height = 9 }, // M
	{ .x = 84, .y = 18, .width = 6, .height = 9 }, // N
	{ .x = 90, .y = 18, .width = 6, .height = 9 }, // O
	{ .x =  0, .y = 27, .width = 6, .height = 9 }, // P
	{ .x =  6, .y = 27, .width = 6, .height = 9 }, // Q
	{ .x = 12, .y = 27, .width = 6, .height = 9 }, // R
	{ .x = 18, .y = 27, .width = 6, .height = 9 }, // S
	{ .x = 24, .y = 27, .width = 6, .height = 9 }, // T
	{ .x = 30, .y = 27, .width = 6, .height = 9 }, // U
	{ .x = 36, .y = 27, .width = 6, .height = 9 }, // V
	{ .x = 42, .y = 27, .width = 6, .height = 9 }, // W
	{ .x = 48, .y = 27, .width = 6, .height = 9 }, // X
	{ .x = 54, .y = 27, .width = 6, .height = 9 }, // Y
	{ .x = 60, .y = 27, .width = 6, .height = 9 }, // Z
	{ .x = 66, .y = 27, .width = 3, .height = 9 }, // [
	{ .x = 72, .y = 27, .width = 6, .height = 9 }, // Backslash
	{ .x = 78, .y = 27, .width = 3, .height = 9 }, // ]
	{ .x = 84, .y = 27, .width = 4, .height = 9 }, // ^
	{ .x = 90, .y = 27, .width = 6, .height = 9 }, // _
	{ .x =  0, .y = 36, .width = 3, .height = 9 }, // `
	{ .x =  6, .y = 36, .width = 6, .height = 9 }, // a
	{ .x = 12, .y = 36, .width = 6, .height = 9 }, // b
	{ .x = 18, .y = 36, .width = 6, .height = 9 }, // c
	{ .x = 24, .y = 36, .width = 6, .height = 9 }, // d
	{ .x = 30, .y = 36, .width = 6, .height = 9 }, // e
	{ .x = 36, .y = 36, .width = 5, .height = 9 }, // f
	{ .x = 42, .y = 36, .width = 6, .height = 9 }, // g
	{ .x = 48, .y = 36, .width = 5, .height = 9 }, // h
	{ .x = 54, .y = 36, .width = 2, .height = 9 }, // i
	{ .x = 60, .y = 36, .width = 4, .height = 9 }, // j
	{ .x = 66, .y = 36, .width = 5, .height = 9 }, // k
	{ .x = 72, .y = 36, .width = 2, .height = 9 }, // l
	{ .x = 78, .y = 36, .width = 6, .height = 9 }, // m
	{ .x = 84, .y = 36, .width = 5, .height = 9 }, // n
	{ .x = 90, .y = 36, .width = 6, .height = 9 }, // o
	{ .x =  0, .y = 45, .width = 6, .height = 9 }, // p
	{ .x =  6, .y = 45, .width = 6, .height = 9 }, // q
	{ .x = 12, .y = 45, .width = 6, .height = 9 }, // r
	{ .x = 18, .y = 45, .width = 6, .height = 9 }, // s
	{ .x = 24, .y = 45, .width = 5, .height = 9 }, // t
	{ .x = 30, .y = 45, .width = 5, .height = 9 }, // u
	{ .x = 36, .y = 45, .width = 6, .height = 9 }, // v
	{ .x = 42, .y = 45, .width = 6, .height = 9 }, // w
	{ .x = 48, .y = 45, .width = 6, .height = 9 }, // x
	{ .x = 54, .y = 45, .width = 6, .height = 9 }, // y
	{ .x = 60, .y = 45, .width = 5, .height = 9 }, // z
	{ .x = 66, .y = 45, .width = 4, .height = 9 }, // {
	{ .x = 72, .y = 45, .width = 2, .height = 9 }, // |
	{ .x = 78, .y = 45, .width = 4, .height = 9 }, // }
	{ .x = 84, .y = 45, .width = 6, .height = 9 }, // ~
	{ .x = 90, .y = 45, .width = 6, .height = 9 }, // Invalid character

	{ .x =  0, .y = 54, .width = 10, .height = 10 }, // D-pad
	{ .x = 10, .y = 54, .width = 10, .height = 10 }, // D-pad X
	{ .x = 20, .y = 54, .width = 10, .height = 10 }, // D-pad Y
	{ .x = 30, .y = 54, .width = 10, .height = 10 }, // Circle
	{ .x = 40, .y = 54, .width = 10, .height = 10 }, // X
	{ .x = 50, .y = 54, .width = 10, .height = 10 }, // Triangle
	{ .x = 60, .y = 54, .width = 10, .height = 10 }, // Square
	{ .x = 70, .y = 54, .width =  7, .height = 10 }, // Select
	{ .x = 80, .y = 54, .width =  7, .height = 10 }  // Start
};

void printString(
	RenderContext *ctx,
	int           x,
	int           y,
	uint32_t      color,
	const char    *str
) {
	if (!str)
		return;

	GPUDMAChain *chain = getCurrentChain(ctx);

	int currentX = x, currentY = y;

	uint32_t *ptr;

	ptr    = allocateGP0Packet(chain, 1);
	ptr[0] = gp0_setPage(ctx->font.page, false, false);

	for (; *str; str++) {
		uint8_t ch = (uint8_t) *str;

		switch (ch) {
			case '\t':
				currentX += FONT_TAB_WIDTH - 1;
				currentX -= currentX % FONT_TAB_WIDTH;
				continue;

			case '\n':
				currentX  = x;
				currentY += FONT_LINE_HEIGHT;
				continue;

			case ' ':
				currentX += FONT_SPACE_WIDTH;
				continue;

			case 0x89 ... 0xff:
				ch = 0x7f;
				break;
		}

		const SpriteInfo *sprite = &fontSprites[ch - FONT_FIRST_TABLE_CHAR];

		ptr    = allocateGP0Packet(chain, 4);
		ptr[0] = color | gp0_rectangle(true, ch >> 7, true);
		ptr[1] = gp0_xy(currentX, currentY);
		ptr[2] = gp0_uv(
			ctx->font.u + sprite->x,
			ctx->font.v + sprite->y,
			ctx->font.clut
		);
		ptr[3] = gp0_xy(sprite->width, sprite->height);

		currentX += sprite->width;
	}
}

int getStringWidth(const char *str) {
	if (!str)
		return 0;

	int currentX = 0, maxWidth = 0;

	for (; *str; str++) {
		// uint8_t, not char. This project builds with -fsigned-char, so the
		// button-glyph bytes (0x80-0x86, see defs.h) came out NEGATIVE here:
		// the 0x89..0xff clamp below never matched them, and
		// fontSprites[ch - FONT_FIRST_TABLE_CHAR] then indexed before the
		// start of the array and added whatever garbage it found to the
		// width.
		//
		// printString() already did this correctly, so measurement and
		// rendering silently disagreed for any string containing a glyph -
		// the memory card Options popup measured 253px for a title that
		// renders 144px, which is why it was drawn about twice as wide as
		// its contents.
		uint8_t ch = (uint8_t) *str;

		switch (ch) {
			case '\t':
				currentX += FONT_TAB_WIDTH - 1;
				currentX -= currentX % FONT_TAB_WIDTH;
				continue;

			case '\n':
				if (currentX > maxWidth)
					maxWidth = currentX;

				currentX = 0;
				continue;

			case ' ':
				currentX += FONT_SPACE_WIDTH;
				continue;

			case 0x89 ... 0xff:
				ch = 0x7f;
				break;
		}

		const SpriteInfo *sprite = &fontSprites[ch - FONT_FIRST_TABLE_CHAR];
		currentX                += sprite->width;
	}

	if (currentX > maxWidth)
		maxWidth = currentX;

	return maxWidth;
}

// scale(n) applies scalePercent to a native glyph-table dimension (n),
// rounding rather than truncating so a run of scaled-down thin glyphs
// (like the D-pad/button glyphs' widths) doesn't visibly drift from the
// unscaled advance over a long string.
static int scaleDim(int n, int scalePercent) {
	return (n * scalePercent + 50) / 100;
}

void printStringScaled(
	RenderContext *ctx,
	int           x,
	int           y,
	uint32_t      color,
	const char    *str,
	int           scalePercent
) {
	if (!str)
		return;

	GPUDMAChain *chain = getCurrentChain(ctx);

	int currentX = x, currentY = y;
	int lineHeight = scaleDim(FONT_LINE_HEIGHT, scalePercent);
	int spaceWidth = scaleDim(FONT_SPACE_WIDTH, scalePercent);
	int tabWidth   = scaleDim(FONT_TAB_WIDTH,   scalePercent);

	uint32_t *ptr;

	ptr    = allocateGP0Packet(chain, 1);
	ptr[0] = gp0_setPage(ctx->font.page, false, false);

	for (; *str; str++) {
		uint8_t ch = (uint8_t) *str;

		switch (ch) {
			case '\t':
				currentX += tabWidth - 1;
				currentX -= currentX % tabWidth;
				continue;

			case '\n':
				currentX  = x;
				currentY += lineHeight;
				continue;

			case ' ':
				currentX += spaceWidth;
				continue;

			case 0x89 ... 0xff:
				ch = 0x7f;
				break;
		}

		const SpriteInfo *sprite = &fontSprites[ch - FONT_FIRST_TABLE_CHAR];

		int dw = scaleDim(sprite->width,  scalePercent);
		int dh = scaleDim(sprite->height, scalePercent);
		if (dw < 1) dw = 1;
		if (dh < 1) dh = 1;

		uint32_t u0 = ctx->font.u + sprite->x, v0 = ctx->font.v + sprite->y;
		uint32_t u1 = u0 + sprite->width,      v1 = v0 + sprite->height;

		// A textured quad, not a Sprite/Rectangle primitive: Sprites are
		// fixed 1:1 texel-to-pixel on PS1 hardware and cannot be scaled at
		// all (setting a smaller destination width/height on one just crops
		// it rather than shrinking it) - a quad's UV is interpolated across
		// whatever screen area its 4 vertices cover, which is what actually
		// achieves scaling. See the comment on printStringScaled() in
		// font.h for the resulting per-glyph cost.
		ptr    = allocateGP0Packet(chain, 9);
		ptr[0] = color | gp0_shadedQuad(false, true, false);
		ptr[1] = gp0_xy(currentX,      currentY);
		ptr[2] = gp0_uv(u0, v0, ctx->font.clut);
		ptr[3] = gp0_xy(currentX + dw, currentY);
		ptr[4] = gp0_uv(u1, v0, ctx->font.page);
		ptr[5] = gp0_xy(currentX,      currentY + dh);
		ptr[6] = gp0_uv(u0, v1, ctx->font.page);
		ptr[7] = gp0_xy(currentX + dw, currentY + dh);
		ptr[8] = gp0_uv(u1, v1, ctx->font.page);

		currentX += dw;
	}
}

int getStringWidthScaled(const char *str, int scalePercent) {
	int native = getStringWidth(str);
	return scaleDim(native, scalePercent);
}
