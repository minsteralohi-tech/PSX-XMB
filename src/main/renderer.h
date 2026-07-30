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

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "common/gpu.h"

typedef struct {
	GPUDMAChain dmaChains[2];
	TextureInfo background, font;

	uint32_t frameCounter;
	uint16_t screenWidth, screenHeight;
} RenderContext;

#ifdef __cplusplus
extern "C" {
#endif

static inline GPUDMAChain *__attribute__((always_inline)) getCurrentChain(
	RenderContext *ctx
) {
	return &ctx->dmaChains[ctx->frameCounter % 2];
}

void setupRenderer(RenderContext *ctx, int width, int height);
void reloadTextures(RenderContext *ctx);
void beginFrame(RenderContext *ctx);
void endFrame(RenderContext *ctx);

void drawBackground(RenderContext *ctx);

// Toggles the scrolling background pattern on/off. When off, the pattern
// freezes at its current position instead of resetting to 0,0 - it just
// stops advancing. Independent of frameCounter, which still needs to keep
// incrementing normally for double-buffering.
void setBackgroundScrollEnabled(bool enabled);
bool isBackgroundScrollEnabled(void);

void drawRect(
	RenderContext *ctx,
	int           x,
	int           y,
	int           width,
	int           height,
	uint32_t      color,
	bool          blend
);
void drawGradientRectH(
	RenderContext *ctx,
	int           x,
	int           y,
	int           width,
	int           height,
	uint32_t      leftColor,
	uint32_t      rightColor,
	bool          blend
);

#ifdef __cplusplus
}
#endif
