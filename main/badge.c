/*
 * PSX-iTests - "Trophy Room" showcase screen (see badge.h)
 */

#include <stdint.h>
#include <stdio.h>
#include "common/gpu.h"
#include "common/sio0.h"
#include "main/badge.h"
#include "main/defs.h"
#include "main/font.h"
#include "main/hud.h"
#include "main/renderer.h"
#include "main/sound.h"
#include "main/trig.h"
#include "ps1/gpucmd.h"

#define WAVE_FULL (1 << (ISIN_SHIFT + 2))   /* 4096, matches xmb_bg.c */

/* --- small local primitive helpers (kept self-contained rather than
 * reaching into xmb_bg.c's statics) -------------------------------------- */

static void setBlendAdd(GPUDMAChain *chain) {
	uint32_t *ptr = allocateGP0Packet(chain, 1);
	ptr[0] = gp0_setPage(
		gp0_page(0, 0, GP0_BLEND_ADD, GP0_COLOR_16BPP), true, false
	);
}
static void setBlendNormal(GPUDMAChain *chain) {
	uint32_t *ptr = allocateGP0Packet(chain, 1);
	ptr[0] = gp0_setPage(
		gp0_page(0, 0, GP0_BLEND_SEMITRANS, GP0_COLOR_16BPP), true, false
	);
}

static void gTri(
	GPUDMAChain *chain,
	int x0, int y0, uint32_t c0,
	int x1, int y1, uint32_t c1,
	int x2, int y2, uint32_t c2,
	bool blend
) {
	uint32_t *ptr = allocateGP0Packet(chain, 6);
	ptr[0] = c0 | gp0_shadedTriangle(true, false, blend);
	ptr[1] = gp0_xy(x0, y0);
	ptr[2] = c1;
	ptr[3] = gp0_xy(x1, y1);
	ptr[4] = c2;
	ptr[5] = gp0_xy(x2, y2);
}

static void gLine(
	GPUDMAChain *chain,
	int x0, int y0, uint32_t c0,
	int x1, int y1, uint32_t c1,
	bool blend
) {
	uint32_t *ptr = allocateGP0Packet(chain, 4);
	ptr[0] = c0 | gp0_line(true, blend);
	ptr[1] = gp0_xy(x0, y0);
	ptr[2] = c1;
	ptr[3] = gp0_xy(x1, y1);
}

static uint32_t scaleRGB(uint32_t c, int num) {
	int r = ( c        & 0xff) * num >> 8;
	int g = ((c >>  8) & 0xff) * num >> 8;
	int b = ((c >> 16) & 0xff) * num >> 8;
	if (r > 255) r = 255;
	if (g > 255) g = 255;
	if (b > 255) b = 255;
	return gp0_rgb(r, g, b);
}

/* Soft additive glow disc: bright centre fading to black at the rim, drawn
 * as a Gouraud triangle fan (same trick as xmb_bg.c's nebulaBlob()). */
static void glowRing(GPUDMAChain *chain, int cx, int cy, int radius, uint32_t centre) {
	const int RINGS = 12;
	uint32_t black = gp0_rgb(0, 0, 0);
	int px = cx + radius, py = cy;
	for (int k = 1; k <= RINGS; k++) {
		int a  = (k * (WAVE_FULL / RINGS)) & (WAVE_FULL - 1);
		int nx = cx + (icos(a) * radius >> 12);
		int ny = cy + (isin(a) * radius >> 12);
		gTri(chain, cx, cy, centre, px, py, black, nx, ny, black, true);
		px = nx; py = ny;
	}
}

/* Filled opaque disc (medal/badge body), inner colour -> outer colour. */
static void filledDisc(
	GPUDMAChain *chain, int cx, int cy, int radius, uint32_t cIn, uint32_t cOut
) {
	const int RINGS = 12;
	int px = cx + radius, py = cy;
	for (int k = 1; k <= RINGS; k++) {
		int a  = (k * (WAVE_FULL / RINGS)) & (WAVE_FULL - 1);
		int nx = cx + (icos(a) * radius >> 12);
		int ny = cy + (isin(a) * radius >> 12);
		gTri(chain, cx, cy, cIn, px, py, cOut, nx, ny, cOut, false);
		px = nx; py = ny;
	}
}

/* --- helper text ---------------------------------------------------------*/

static int textLeftFor(const char *s, int centreX) {
	int n = 0;
	while (s[n]) n++;
	return centreX - (n * 6) / 2;
}

/* One badge medal in the original style: a soft glow halo, radiating
 * starburst rays, a pulsing gold rim around a dark inner disc, the short
 * badge name INSIDE the disc, and a full descriptive label underneath.
 * Locked badges get a dim, ray-less, non-pulsing version. */
static void drawBadgeSlot(
	GPUDMAChain *chain, RenderContext *ctx,
	int cx, int cy, bool unlocked, uint32_t frame,
	const char *inner, const char *sub
) {
	const int R = 26;   // rim radius

	if (unlocked) {
		int pulse = 200 + (isin((frame * 6) & (WAVE_FULL - 1)) * 55 >> 12);
		uint32_t gold     = gp0_rgb(255, 205, 60);
		uint32_t goldDeep = gp0_rgb(150, 100, 10);
		uint32_t goldGlow = scaleRGB(gold, pulse);

		// soft glow halo behind everything
		setBlendAdd(chain);
		glowRing(chain, cx, cy, R + 14, scaleRGB(gold, pulse / 2));

		// radiating starburst rays, slowly rotating, lengths shimmering
		int spin = (frame * 8) & (WAVE_FULL - 1);
		for (int i = 0; i < 8; i++) {
			int a = spin + i * (WAVE_FULL / 8);
			int len = R + 12 + (isin((a * 3 + frame * 20) & (WAVE_FULL - 1)) * 10 >> 12);
			int x1 = cx + (icos(a) * len >> 12);
			int y1 = cy + (isin(a) * len >> 12);
			gLine(chain, cx, cy, scaleRGB(gp0_rgb(255, 235, 170), pulse),
				x1, y1, gp0_rgb(0, 0, 0), true);
		}

		// gold rim + dark inner face
		setBlendNormal(chain);
		filledDisc(chain, cx, cy, R, goldGlow, goldDeep);
		filledDisc(chain, cx, cy, R - 7, gp0_rgb(30, 22, 8), gp0_rgb(14, 10, 4));
	} else {
		// locked: dim, no rays, no pulse
		setBlendAdd(chain);
		glowRing(chain, cx, cy, R + 6, gp0_rgb(24, 24, 30));
		setBlendNormal(chain);
		filledDisc(chain, cx, cy, R, gp0_rgb(70, 70, 78), gp0_rgb(30, 30, 36));
		filledDisc(chain, cx, cy, R - 7, gp0_rgb(18, 18, 22), gp0_rgb(10, 10, 12));
	}

	// short name INSIDE the disc
	printString(ctx, textLeftFor(inner, cx), cy - 3,
		unlocked ? 0xffffff : 0x808080, inner);

	// full descriptive label underneath
	printString(ctx, textLeftFor(sub, cx), cy + R + 6,
		unlocked ? 0xffe090 : 0x707070, sub);
}

/* --- screen ---------------------------------------------------------------
 *
 * Shows all four badge slots (locked/unlocked), the running count, and
 * whichever feature tiers that count has unlocked so far.
 */
void runTrophyRoom(RenderContext *ctx, UIState *state, const MenuItem *item) {
	(void) state;
	(void) item;

	uint32_t frame = 0;
	int cx = ctx->screenWidth / 2;

	// Debounce the button that opened this screen.
	while (pollController(0) | pollController(1))
		;

	for (;;) {
		uint16_t pad = pollController(0) | pollController(1);
		if (pad & PAD_BTN_CIRCLE) {
			playCancelSound();
			break;
		}

		beginFrame(ctx);

		// The Trophy screen draws its own cheap backdrop rather than the full
		// XMB theme: the four medals (each with a starburst) plus all the
		// text already use a big slice of the 4096-word GPU chain, and the
		// heaviest themes (Nebula) would push the combined frame over the
		// limit and crash. A dark vertical gradient + a few twinkling stars
		// keeps it on-theme for cheap.
		{
			GPUDMAChain *bg = getCurrentChain(ctx);
			int w = ctx->screenWidth, hh = ctx->screenHeight;
			uint32_t *q = allocateGP0Packet(bg, 8);
			uint32_t top = gp0_rgb(10, 12, 30), bot = gp0_rgb(4, 4, 10);
			q[0] = top | gp0_shadedQuad(true, false, false);
			q[1] = gp0_xy(0, 0);   q[2] = top; q[3] = gp0_xy(w, 0);
			q[4] = bot; q[5] = gp0_xy(0, hh);  q[6] = bot; q[7] = gp0_xy(w, hh);
		}

		GPUDMAChain *chain = getCurrentChain(ctx);
		frame++;

		bool b8   = isBadge8MBRAM();
		bool b16  = isBadge16MBRAM();
		bool bV   = isBadge2MBVRAM();
		bool bSPU = isBadgeSPUVerified();
		int  count = getUnlockedBadgeCount();

		printString(ctx, textLeftFor("TROPHY ROOM", cx), 12, 0xffffff, "TROPHY ROOM");

		char countLine[16];
		snprintf(countLine, sizeof(countLine), "%d / 4", count);
		printString(ctx, textLeftFor(countLine, cx), 26, 0xc0c0c0, countLine);

		// (Each unlocked badge draws its own starburst; the Feature-1 payoff
		// is that those rays and the gold pulse only appear once you've
		// earned at least one badge.)

		int slotY = 74;
		drawBadgeSlot(chain, ctx, cx - 108, slotY, b8,   frame, "8MB",  "8MB RAM");
		drawBadgeSlot(chain, ctx, cx - 36,  slotY, b16,  frame, "16MB", "16MB RAM");
		drawBadgeSlot(chain, ctx, cx + 36,  slotY, bV,   frame, "VRAM", "2MB VRAM");
		drawBadgeSlot(chain, ctx, cx + 108, slotY, bSPU, frame, "SPU",  "SPU Test");

		printString(ctx, 16, 138, 0x808080,
			isFeature1Unlocked()
				? "Feature 1 UNLOCKED: Trophy Shine"
				: "Feature 1: complete 1 badge to unlock");

		printString(ctx, 16, 150, 0x808080,
			isFeature2Unlocked()
				? "Feature 2: ready (coming in a future update)"
				: "Feature 2: complete 3 badges to unlock");

		printString(ctx, 16, 168, 0x606060, "8MB/16MB RAM: detected automatically.");
		printString(ctx, 16, 178, 0x606060, "VRAM: set VRAM size to 2MB in the tester.");
		printString(ctx, 16, 188, 0x606060, "SPU: run and pass the SPU RAM test.");

		printString(ctx, 16, 214, 0x606060, CH_PS1_CIRCLE_BUTTON " Back");

		endFrame(ctx);
	}

	while (pollController(0) | pollController(1))
		;
}
