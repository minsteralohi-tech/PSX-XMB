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

#include <stdbool.h>
#include <stdint.h>
#include "common/gpu.h"
#include "main/icon.h"
#include "main/renderer.h"
#include "main/xmb_bg.h"
#include "ps1/registers.h"

#define BG_WIDTH         96
#define BG_HEIGHT        48
#define BG_COLOR_DEPTH   GP0_COLOR_4BPP
#define FONT_WIDTH       96
#define FONT_HEIGHT      64
#define FONT_COLOR_DEPTH GP0_COLOR_4BPP

extern const uint8_t bgTexture[],   bgPalette[];
extern const uint8_t fontTexture[], fontPalette[];

/* Renderer management */

void setupRenderer(RenderContext *ctx, int width, int height) {
	ctx->frameCounter = 0;
	ctx->screenWidth  = width;
	ctx->screenHeight = height;

	reloadTextures(ctx);
}

void reloadTextures(RenderContext *ctx) {
	int textureX = ctx->screenWidth * 2;

	uploadIndexedTexture(
		&ctx->background,
		bgTexture,
		bgPalette,
		textureX,
		0,
		textureX,
		BG_HEIGHT + FONT_HEIGHT,
		BG_WIDTH,
		BG_HEIGHT,
		BG_COLOR_DEPTH
	);
	uploadIndexedTexture(
		&ctx->font,
		fontTexture,
		fontPalette,
		textureX,
		BG_HEIGHT,
		textureX + 16,
		BG_HEIGHT + FONT_HEIGHT,
		FONT_WIDTH,
		FONT_HEIGHT,
		FONT_COLOR_DEPTH
	);

	// The menu icon sheets live in VRAM too, so anything that reloads the
	// base textures must restore them as well. The VRAM tester overwrites
	// the whole of VRAM while testing and then calls reloadTextures() to
	// recover - without this the icons came back as garbage afterwards.
	initIcons(ctx);

	// Same story for the Nebula themes' planet textures (sun/lava/earth/
	// moon) - added after this function was first written, and never
	// wired in here. This was the actual, complete cause of "Nebula 2/3's
	// textures come back as garbage after running the VRAM test": the
	// GPU addressing-mode bug (GP1 VRAM size not being restored, fixed in
	// ramtester.c) was real, but even with that fixed, the VRAM test still
	// deliberately overwrites the ENTIRE VRAM contents with random test
	// patterns and relies entirely on this function to restore every
	// texture afterward - and this function simply didn't know these
	// four existed.
	initNebulaTexture(ctx);
}

void beginFrame(RenderContext *ctx) {
	int bufferX = (ctx->frameCounter % 2) ? ctx->screenWidth : 0;
	int bufferY = 0;

	GPUDMAChain *chain = getCurrentChain(ctx);
	chain->nextPacket  = chain->data;

	GPU_GP1 = gp1_fbOffset(bufferX, bufferY);

	uint32_t *ptr;

	ptr    = allocateGP0Packet(chain, 4);
	ptr[0] = gp0_setPage(0, true, false);
	ptr[1] = gp0_fbOffset1(bufferX, bufferY);
	ptr[2] = gp0_fbOffset2(
		bufferX + ctx->screenWidth  - 1,
		bufferY + ctx->screenHeight - 1
	);
	ptr[3] = gp0_fbOrigin(bufferX, bufferY);

#if 0
	ptr    = allocateGP0Packet(chain, 3);
	ptr[0] = gp0_rgb(0, 0, 0) | gp0_vramFill();
	ptr[1] = gp0_xy(bufferX, bufferY);
	ptr[2] = gp0_xy(ctx->screenWidth, ctx->screenHeight);
#endif
}

void endFrame(RenderContext *ctx) {
	GPUDMAChain *chain   = getCurrentChain(ctx);
	*(chain->nextPacket) = gp0_endTag(0);

	waitForGP0Ready();
	waitForVSync();
	sendGPULinkedList(chain->data);

	ctx->frameCounter++;
}

/* Drawing functions */

static bool     backgroundScrollEnabled = true;
static uint32_t backgroundScrollFrame   = 0;

void setBackgroundScrollEnabled(bool enabled) {
	backgroundScrollEnabled = enabled;
}

bool isBackgroundScrollEnabled(void) {
	return backgroundScrollEnabled;
}

void drawBackground(RenderContext *ctx) {
	GPUDMAChain       *chain = getCurrentChain(ctx);
	const TextureInfo *image = &ctx->background;

	if (backgroundScrollEnabled)
		backgroundScrollFrame++;

	int offsetX  = (backgroundScrollFrame / 2) % image->width;
	int offsetY  = (backgroundScrollFrame / 3) % (image->height * 2);
	int staggerX = image->width / 2;

	uint32_t *ptr;

	ptr    = allocateGP0Packet(chain, 1);
	ptr[0] = gp0_setPage(image->page, false, false);

	for (int y = -offsetY; y < ctx->screenHeight; y += image->height) {
		for (int x = -offsetX; x < ctx->screenWidth; x += image->width) {
			ptr    = allocateGP0Packet(chain, 4);
			ptr[0] = gp0_rectangle(true, true, false);
			ptr[1] = gp0_xy(x, y);
			ptr[2] = gp0_uv(image->u, image->v, image->clut);
			ptr[3] = gp0_xy(image->width, image->height);
		}

		offsetX +=  staggerX;
		staggerX = -staggerX;
	}
}

void drawRect(
	RenderContext *ctx,
	int           x,
	int           y,
	int           width,
	int           height,
	uint32_t      color,
	bool          blend
) {
	GPUDMAChain *chain = getCurrentChain(ctx);

	uint32_t *ptr;

	ptr    = allocateGP0Packet(chain, 3);
	ptr[0] = color | gp0_rectangle(false, false, blend);
	ptr[1] = gp0_xy(x, y);
	ptr[2] = gp0_xy(width, height);
}

void drawGradientRectH(
	RenderContext *ctx,
	int           x,
	int           y,
	int           width,
	int           height,
	uint32_t      leftColor,
	uint32_t      rightColor,
	bool          blend
) {
	GPUDMAChain *chain = getCurrentChain(ctx);

	uint32_t *ptr;

	ptr    = allocateGP0Packet(chain, 9);
	ptr[0] = gp0_setPage(0, true, false);
	ptr[1] = leftColor | gp0_shadedQuad(true, false, blend);
	ptr[2] = gp0_xy(x, y);
	ptr[3] = rightColor;
	ptr[4] = gp0_xy(x + width, y);
	ptr[5] = leftColor;
	ptr[6] = gp0_xy(x, y + height);
	ptr[7] = rightColor;
	ptr[8] = gp0_xy(x + width, y + height);
}
