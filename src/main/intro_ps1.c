/*
 * PSX-iTests - PlayStation boot sequence
 *
 * A recreation of the original PS1 startup, timed from the HTML/CSS/jQuery
 * remake in PS1_Startup_Remake-1.0.0. Every delay below is the one in that
 * project's js/startup.js, minus its opening second: that first second was
 * the demo page fading its own background from black to white before the
 * animation proper, which this does not need - the sequence starts on white.
 *
 * Dropping it also removes the one effect this hardware cannot do well. A
 * fade UP to white has no cheap equivalent: the GPU's semi-transparent blend
 * only ever mixes toward what is already there, so darkening is easy and
 * brightening is not. Starting on white sidesteps it entirely.
 *
 * TIMELINE (60Hz frames; ~15.65s total)
 *
 *      0   white screen, SCE diamond fades in over 6f
 *      9   two triangles converge for 51f, shrinking 25%x50% -> 10%x20%
 *     60   SONY wordmark and COMPUTER/ENTERTAINMENT fade in over 18f
 *    360   SCE screen fades out over 6f
 *    372   background switches to black
 *    375   PS logo fades in over 27f
 *    402   credits fade in
 *    405   PlayStation wordmark fades in over 24f
 *    870   everything fades out over 9f
 *    939   ends, menu takes over
 *
 * TWO KINDS OF FADE, FOR A REASON
 *
 * The SCE screen is dark artwork on white, so its fades are done by drawing
 * the artwork blended one or more times: one pass reads as 50% grey, two as
 * 75%, three as 87.5%. Coarse, but it is the only direction the blend works
 * in, and on a white field the steps are hard to see.
 *
 * The PlayStation screen is bright artwork on black, so its fades scale the
 * vertex colour instead - the GPU modulates each texel by it, giving a
 * genuinely smooth 0..255 ramp. Same reason the intro.c wave fade had to be
 * stepped while this one does not.
 */

#include <stdbool.h>
#include <stdint.h>
#include "common/sio0.h"
#include "main/font.h"
#include "main/intro_ps1.h"
#include "main/renderer.h"

extern const uint8_t introSonyTexture[],   introSonyPalette[];
extern const uint8_t introSceTextTexture[], introSceTextPalette[];
extern const uint8_t introPsLogoTexture[], introPsLogoPalette[];
extern const uint8_t introPsTextTexture[], introPsTextPalette[];

/*
 * VRAM slots.
 *
 * x=960..1000, stacked down y. The pad glyph sheet holds 960..996 at y0..64,
 * so these all start at y=64 and below. Everything stays inside one 64-column
 * texture page whose Y base is 0, which is why no sprite crosses y=256.
 *
 * See icon.c's PAD_GLYPH_VRAM_X note before reusing anything in 832..960 -
 * the planet textures live there for the full height of VRAM.
 */
#define SONY_W 160
#define SONY_H  28
#define SCET_W 122
#define SCET_H  24
#define PSLG_W  90
#define PSLG_H  82
#define PSTX_W  73
#define PSTX_H  16

#define INTRO_VRAM_X 960
#define SONY_VRAM_Y   64
#define SCET_VRAM_Y   96
#define PSLG_VRAM_Y  128
#define PSTX_VRAM_Y  216

#define SONY_CLUT_X 752
#define SCET_CLUT_X 768
#define PSLG_CLUT_X 784
#define PSTX_CLUT_X 800
#define INTRO_CLUT_Y 256

static TextureInfo sonyTex, sceTextTex, psLogoTex, psTextTex;

/* --- timeline, in frames --------------------------------------------- */
#define T_DIAMOND_IN     0
#define T_DIAMOND_IN_END 6
#define T_TRI_START      9
#define T_TRI_END       60
#define T_SCE_TEXT      60
#define T_SCE_TEXT_END  78
#define T_SCE_OUT      360
#define T_SCE_OUT_END  366
#define T_TO_BLACK     372
#define T_PS_LOGO      375
#define T_PS_LOGO_END  402
#define T_PS_CREDITS   402
#define T_PS_TEXT      405
#define T_PS_TEXT_END  429
#define T_ALL_OUT      870
#define T_ALL_OUT_END  879
#define T_END          939

/*
 * The CSS lays the whole animation out inside a square (100vmin), so on a
 * 320x240 screen the reference square is 240x240, centred: x 40..280.
 * Everything below is a percentage of that square, not of the full width.
 */
#define SQ      240
#define SQ_X     40
#define SQ_Y      0

#define PCT_X(p) (SQ_X + (SQ * (p)) / 100)
#define PCT_Y(p) (SQ_Y + (SQ * (p)) / 100)

static int clampi(int v, int lo, int hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

/* Linear interpolation over a frame window, returning 0..256. */
static int ramp(int frame, int start, int end) {
	if (frame <= start)
		return 0;
	if (frame >= end)
		return 256;

	return (frame - start) * 256 / (end - start);
}

/* --- primitives ------------------------------------------------------- */

/*
 * Flat-shaded quad with a per-vertex colour: the diamond and both triangles
 * are drawn with this. Vertex order is TL, TR, BL, BR as the GPU expects,
 * and a "triangle" is just a quad with two coincident corners.
 */
static void gouraudQuad(
	RenderContext *ctx,
	int x0, int y0, uint32_t c0,
	int x1, int y1, uint32_t c1,
	int x2, int y2, uint32_t c2,
	int x3, int y3, uint32_t c3
) {
	GPUDMAChain *chain = getCurrentChain(ctx);
	uint32_t *ptr = allocateGP0Packet(chain, 8);

	ptr[0] = c0 | gp0_shadedQuad(true, false, false);
	ptr[1] = gp0_xy(x0, y0);
	ptr[2] = c1;
	ptr[3] = gp0_xy(x1, y1);
	ptr[4] = c2;
	ptr[5] = gp0_xy(x2, y2);
	ptr[6] = c3;
	ptr[7] = gp0_xy(x3, y3);
}

/* Textured sprite at native size, modulated by `tint` and optionally blended. */
static void sprite(
	RenderContext *ctx, const TextureInfo *tex,
	int x, int y, int w, int h, uint32_t tint, bool blend
) {
	GPUDMAChain *chain = getCurrentChain(ctx);

	uint32_t *page = allocateGP0Packet(chain, 1);
	page[0] = gp0_setPage(tex->page, false, false);

	uint32_t *ptr = allocateGP0Packet(chain, 9);
	ptr[0] = tint | gp0_shadedQuad(false, true, blend);
	ptr[1] = gp0_xy(x, y);
	ptr[2] = gp0_uv(tex->u, tex->v, tex->clut);
	ptr[3] = gp0_xy(x + w, y);
	ptr[4] = gp0_uv(tex->u + w - 1, tex->v, tex->page);
	ptr[5] = gp0_xy(x, y + h);
	ptr[6] = gp0_uv(tex->u, tex->v + h - 1, 0);
	ptr[7] = gp0_xy(x + w, y + h);
	ptr[8] = gp0_uv(tex->u + w - 1, tex->v + h - 1, 0);
}

/*
 * Dark artwork fading in on a light background: draw it blended, repeatedly.
 * Each pass halves the remaining background showing through, so 1..3 passes
 * give 50%, 75%, 87.5% and the fourth is effectively solid.
 */
static void spriteFadeOnLight(
	RenderContext *ctx, const TextureInfo *tex,
	int x, int y, int w, int h, int level
) {
	if (level <= 0)
		return;

	int passes = clampi((level * 4) / 256, 0, 4);

	for (int i = 0; i < passes; i++)
		sprite(ctx, tex, x, y, w, h, 0x808080, i < 3);
}

void initPS1Boot(RenderContext *ctx) {
	(void) ctx;

	uploadIndexedTexture(&sonyTex, introSonyTexture, introSonyPalette,
		INTRO_VRAM_X, SONY_VRAM_Y, SONY_CLUT_X, INTRO_CLUT_Y,
		SONY_W, SONY_H, GP0_COLOR_4BPP);

	uploadIndexedTexture(&sceTextTex, introSceTextTexture, introSceTextPalette,
		INTRO_VRAM_X, SCET_VRAM_Y, SCET_CLUT_X, INTRO_CLUT_Y,
		SCET_W, SCET_H, GP0_COLOR_4BPP);

	uploadIndexedTexture(&psLogoTex, introPsLogoTexture, introPsLogoPalette,
		INTRO_VRAM_X, PSLG_VRAM_Y, PSLG_CLUT_X, INTRO_CLUT_Y,
		PSLG_W, PSLG_H, GP0_COLOR_4BPP);

	uploadIndexedTexture(&psTextTex, introPsTextTexture, introPsTextPalette,
		INTRO_VRAM_X, PSTX_VRAM_Y, PSTX_CLUT_X, INTRO_CLUT_Y,
		PSTX_W, PSTX_H, GP0_COLOR_4BPP);
}

/* --- the SCE (white) screen ------------------------------------------- */

static void drawSceScreen(RenderContext *ctx, int frame) {
	// The CSS gradient is horizontal: #E01705 at both edges, #DF9300 in the
	// middle. On a rhombus whose left and right corners sit at the edges and
	// whose top and bottom corners sit at the centre, that is exactly a
	// four-vertex Gouraud fill - no gradient texture needed.
	const uint32_t edge   = 0x0517e0;   /* 0xBBGGRR of #E01705 */
	const uint32_t centre = 0x0093df;   /* 0xBBGGRR of #DF9300 */

	int cx = SQ_X + SQ / 2;
	int cy = SQ_Y + SQ / 2;
	int r  = SQ / 4;                    /* 50% box -> 25% half-extent */

	int level = ramp(frame, T_DIAMOND_IN, T_DIAMOND_IN_END);

	if (level > 0) {
		// Fading a solid shape in on white: interpolate its colour from
		// white toward the real one, which is smooth and costs nothing.
		uint32_t e = edge, c = centre;

		if (level < 256) {
			int k = level;
			uint32_t er = ((edge        & 0xff) * k + 255 * (256 - k)) >> 8;
			uint32_t eg = (((edge >> 8)  & 0xff) * k + 255 * (256 - k)) >> 8;
			uint32_t eb = (((edge >> 16) & 0xff) * k + 255 * (256 - k)) >> 8;
			uint32_t cr = ((centre        & 0xff) * k + 255 * (256 - k)) >> 8;
			uint32_t cg = (((centre >> 8)  & 0xff) * k + 255 * (256 - k)) >> 8;
			uint32_t cb = (((centre >> 16) & 0xff) * k + 255 * (256 - k)) >> 8;
			e = (eb << 16) | (eg << 8) | er;
			c = (cb << 16) | (cg << 8) | cr;
		}

		// TL, TR, BL, BR order: top and bottom points are the centre colour,
		// left and right points the edge colour.
		gouraudQuad(ctx,
			cx - r, cy,     e,
			cx,     cy - r, c,
			cx,     cy + r, c,
			cx + r, cy,     e);
	}

	/*
	 * The two triangles. In the source they start as 25%x50% boxes sitting
	 * either side of centre and animate to 10%x20% while moving inward, so
	 * they read as the diamond's highlight splitting and converging.
	 */
	if (frame >= T_TRI_START) {
		int k = ramp(frame, T_TRI_START, T_TRI_END);

		int wStart = (SQ * 25) / 100, wEnd = (SQ * 10) / 100;
		int hStart = (SQ * 50) / 100, hEnd = (SQ * 20) / 100;

		int w = wStart + ((wEnd - wStart) * k) / 256;
		int h = hStart + ((hEnd - hStart) * k) / 256;

		// Left triangle: clip-path polygon(0% 50%, 100% 100%, 100% 0) - a
		// point on the left, flat edge on the right.
		gouraudQuad(ctx,
			cx - w, cy,         edge,
			cx,     cy - h / 2, centre,
			cx,     cy + h / 2, centre,
			cx,     cy,         centre);

		// Right triangle: mirrored.
		gouraudQuad(ctx,
			cx,     cy - h / 2, centre,
			cx + w, cy,         edge,
			cx,     cy,         centre,
			cx,     cy + h / 2, centre);
	}

	/* SONY wordmark and the COMPUTER / ENTERTAINMENT block. */
	int textLevel = ramp(frame, T_SCE_TEXT, T_SCE_TEXT_END);

	if (frame >= T_SCE_OUT)
		textLevel = 256 - ramp(frame, T_SCE_OUT, T_SCE_OUT_END) ;

	if (textLevel > 0) {
		spriteFadeOnLight(ctx, &sonyTex,
			SQ_X + (SQ - SONY_W) / 2, PCT_Y(5), SONY_W, SONY_H, textLevel);

		spriteFadeOnLight(ctx, &sceTextTex,
			SQ_X + (SQ - SCET_W) / 2, PCT_Y(94) - SCET_H,
			SCET_W, SCET_H, textLevel);

		// The trademark mark, drawn in the dashboard's own font: it is two
		// characters at 3vmin and rasterising an asset for it would cost
		// more than it is worth.
		printString(ctx, cx + (SQ * 11) / 100, PCT_Y(72), 0x202020, "TM");
	}
}

/* --- the PlayStation (black) screen ----------------------------------- */

static void drawPsScreen(RenderContext *ctx, int frame) {
	int logoLevel    = ramp(frame, T_PS_LOGO, T_PS_LOGO_END);
	int textLevel    = ramp(frame, T_PS_TEXT, T_PS_TEXT_END);
	int creditsLevel = frame >= T_PS_CREDITS ? 256 : 0;

	if (frame >= T_ALL_OUT) {
		int out = 256 - ramp(frame, T_ALL_OUT, T_ALL_OUT_END);

		logoLevel    = (logoLevel    * out) >> 8;
		textLevel    = (textLevel    * out) >> 8;
		creditsLevel = (creditsLevel * out) >> 8;
	}

	// Bright artwork on black: scaling the vertex colour gives a genuinely
	// smooth fade, because the GPU modulates every texel by it.
	if (logoLevel > 0) {
		uint32_t g = (uint32_t) clampi((logoLevel * 128) / 256, 0, 128);
		uint32_t tint = (g << 16) | (g << 8) | g;

		sprite(ctx, &psLogoTex,
			SQ_X + (SQ - PSLG_W) / 2, PCT_Y(16), PSLG_W, PSLG_H, tint, false);
	}

	if (textLevel > 0) {
		uint32_t g = (uint32_t) clampi((textLevel * 128) / 256, 0, 128);
		uint32_t tint = (g << 16) | (g << 8) | g;

		sprite(ctx, &psTextTex,
			SQ_X + (SQ - PSTX_W) / 2, PCT_Y(47), PSTX_W, PSTX_H, tint, false);
	}

	if (creditsLevel > 0) {
		uint32_t c = (uint32_t) clampi((creditsLevel * 255) / 256, 0, 255);
		uint32_t col = (c << 16) | (c << 8) | c;

		static const char *const lines[] = {
			"Licensed by",
			"Sony Computer Entertainment Inc.",
		};

		for (int i = 0; i < 2; i++) {
			int w = getStringWidth(lines[i]);
			printString(ctx, (ctx->screenWidth - w) / 2,
				PCT_Y(61) + i * 12, col, lines[i]);
		}
	}
}

void runPS1Boot(RenderContext *ctx) {
	for (int frame = 0; frame < T_END; frame++) {
		if (pollController(0) | pollController(1))
			break;

		beginFrame(ctx);

		bool white = frame < T_TO_BLACK;

		drawRect(ctx, 0, 0, ctx->screenWidth, ctx->screenHeight,
			white ? 0xffffff : 0x000000, false);

		if (white) {
			int out = frame >= T_SCE_OUT
			        ? 256 - ramp(frame, T_SCE_OUT, T_SCE_OUT_END)
			        : 256;

			if (out > 0)
				drawSceScreen(ctx, frame);
		} else {
			drawPsScreen(ctx, frame);
		}

		endFrame(ctx);
	}

	// Do not pass a held button straight to the menu.
	while (pollController(0) | pollController(1))
		;
}
