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

#pragma once

#include <stdint.h>
#include "common/gpu.h"
#include "main/renderer.h"

#define FONT_FIRST_TABLE_CHAR '!'
#define FONT_SPACE_WIDTH       4
#define FONT_TAB_WIDTH        32
#define FONT_LINE_HEIGHT      10

#ifdef __cplusplus
extern "C" {
#endif

void printString(
	RenderContext *ctx,
	int           x,
	int           y,
	uint32_t      color,
	const char    *str
);

// Same as printString(), but each glyph is drawn at scalePercent% of its
// native size (e.g. 70 for 70%). Costs more per character than printString()
// - a textured quad (9 words) instead of a Sprite/Rectangle primitive
// (4 words), since PS1 Sprite primitives are fixed 1:1 texel-to-pixel and
// can't be shrunk or stretched at all, only quads can - so use this only for
// short strings (titles, button-prompt bars), not for large blocks of text.
void printStringScaled(
	RenderContext *ctx,
	int           x,
	int           y,
	uint32_t      color,
	const char    *str,
	int           scalePercent
);

int getStringWidth(const char *str);
// Width of str if drawn with printStringScaled() at scalePercent%.
int getStringWidthScaled(const char *str, int scalePercent);

/* Pad tester direction glyph selected by printDpadDirection(). */
typedef enum {
	DPAD_DIR_UP = 0,
	DPAD_DIR_RIGHT,
	DPAD_DIR_DOWN,
	DPAD_DIR_LEFT
} DpadDirection;

/*
 * Draw the corresponding pad tester direction glyph at (x, y) in its baked
 * neutral colour.
 *
 * "\x89" typed directly into a printString() string renders Up; call this
 * function directly for Right/Down/Left.
 */
void printDpadDirection(
	RenderContext *ctx, int x, int y, DpadDirection direction
);

#ifdef __cplusplus
}
#endif
