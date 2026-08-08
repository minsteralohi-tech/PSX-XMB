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
#include <stdio.h>
#include "common/sio0.h"
#include "main/defs.h"
#include "main/font.h"
#include "main/intro_ps1.h"
#include "main/renderer.h"
#include "main/sound.h"
#include "main/trig.h"
#include "main/xmb_bg.h"

extern const uint8_t introSonyTexture[],   introSonyPalette[];
extern const uint8_t introComputerTexture[], introComputerPalette[];
extern const uint8_t introEnterTexture[],    introEnterPalette[];
extern const uint8_t introTmTexture[],       introTmPalette[];
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
/*
 * Dimensions are padded, and BOTH constraints matter.
 *
 * 1. Width must be a multiple of 4. uploadIndexedTexture() uploads width/4
 *    VRAM columns at 4bpp, so anything else silently loses a column.
 *
 * 2. (width/4 * height / 2) must be a whole number of 16-word DMA chunks.
 *    sendVRAMData() computes numChunks = length / 16 and DROPS the
 *    remainder - it only asserts, and asserts are compiled out in release.
 *    A short transfer leaves the GPU parked in the middle of its
 *    vramWrite command, waiting for pixels that never arrive, so every GP0
 *    command after it is swallowed as texture data and the screen stays
 *    black with the music still playing. That was this sequence's original
 *    black-screen bug, and three of these four textures had it.
 *
 * Padding width to a multiple of 16 and height to a multiple of 8 satisfies
 * both at once. The padding is fully transparent, so nothing is drawn.
 *
 * DRAW sizes are separate: the sprites below draw only the real artwork
 * area, not the padding.
 */
#define SONY_W 128   /* padded; artwork is 115x19 */
#define SONY_H  24
#define COMP_W 128   /* padded; artwork is 120x11 */
#define COMP_H  16
#define ENTR_W 128   /* padded; artwork is 124x10 */
#define ENTR_H  16
#define TM_W    16   /* padded; artwork is 14x7   */
#define TM_H    16
#define PSLG_W  96   /* padded; artwork is 90x82  */
#define PSLG_H  88
#define PSTX_W 112   /* padded; artwork is 107x21 */
#define PSTX_H  24

/*
 * Visible artwork inside each padded texture.
 *
 * The four SCE wordmarks were re-cut to the sizes measured off a real BIOS
 * screen rather than the ones the CSS remake implied. The old SONY in
 * particular was 160 wide - wider than the diamond - where the real one is
 * narrower than it, and COMPUTER was 16 tall against ENTERTAINMENT's 11, so
 * the two lines did not even match each other and overlapped on screen.
 *
 * They are also antialiased now; see the note above initPS1Boot().
 */
#define SONY_DW 115
#define SONY_DH  19
#define COMP_DW 120
#define COMP_DH  11
#define ENTR_DW 124
#define ENTR_DH  10
#define TM_DW    14
#define TM_DH     7
#define PSLG_DW  90
#define PSLG_DH  82
/*
 * The PlayStation wordmark was 80x18 and looked soft next to the dashboard's
 * own hard-edged bitmap font on the same screen: at that width the logotype's
 * strokes came out around two pixels, so most of the word was antialiasing
 * rather than ink. 96 across is nearer what the BIOS shows and gives the
 * letterforms enough pixels to be crisp; its master's aspect is preserved, and
 * the generator hardens this one's edges (CONTRAST_HARD) where the SCE
 * wordmarks stay linear.
 *
 * The trademark mark is part of this texture rather than a sprite of its own.
 * The BIOS screen has a small raised TM right after the wordmark, and building
 * it into the master means no second VRAM slot, no second CLUT and no second
 * draw - the pair is one sprite, centred as a unit. tools/make_intro_wordmarks
 * reads assets/orig_intro_pstext.png, which is the supplied wordmark
 * (kept unmodified as orig_intro_pstext_word.png) with the TM composited on.
 */
#define PSTX_DW 107
#define PSTX_DH  21

#define INTRO_VRAM_X 960
#define SONY_VRAM_Y   64
#define COMP_VRAM_Y   96
#define ENTR_VRAM_Y  112
#define TM_VRAM_Y    240
#define PSLG_VRAM_Y  128
#define PSTX_VRAM_Y  216

#define SONY_CLUT_X 752
#define COMP_CLUT_X 768
#define ENTR_CLUT_X 816
#define TM_CLUT_X   832
#define PSLG_CLUT_X 784
#define PSTX_CLUT_X 800
#define INTRO_CLUT_Y 256

static TextureInfo sonyTex, computerTex, enterTex, tmTex, psLogoTex, psTextTex;

/* Which look to use, chosen from the boot menu. See PS1IntroVariant. */
static int introVariant = PS1_INTRO_CLASSIC;

/*
 * Spin support.
 *
 * The whole mark - diamond and both triangles - is rotated about the screen
 * centre as a flat object. Rotating the vertices rather than doing anything
 * three-dimensional keeps the shape and the gradients exactly as they are;
 * it is the same picture, turned.
 *
 * introSpin is 0 for every variant except Spin, where it runs through one
 * full turn across the same window the triangles animate in.
 */
static int introSpin;

static void rotatePoint(int cx, int cy, int *x, int *y) {
	if (!introSpin)
		return;

	int dx = *x - cx, dy = *y - cy;
	int s = isin(introSpin), c = icos(introSpin);

	*x = cx + ((dx * c - dy * s) >> ISIN_SHIFT);
	*y = cy + ((dx * s + dy * c) >> ISIN_SHIFT);
}


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
 * 320x240 screen the reference square would be 240x240, centred: x 40..280.
 * Everything below is a percentage of that box, not of the full width.
 *
 * It is a rectangle rather than a square here. Measured against a real BIOS
 * screen the mark is 127 wide and 135 tall, centred three pixels above the
 * middle of the frame - not the 120x120 at dead centre the CSS gives. Widening
 * and heightening the reference box scales the diamond AND the two triangles
 * together, which is the point: they are all laid out as percentages of it, so
 * the notches stay exactly where they belong.
 */
/*
 * The SCE screen's field colour.
 *
 * The real BIOS screen is not white - it is a flat mid grey, with the
 * wordmarks in a dark navy rather than black. Both values are sampled off the
 * reference screen.
 *
 * THIS MUST MATCH tools/make_intro_wordmarks.py's FIELD. The wordmarks'
 * antialiasing is baked as opaque pixels ramping from the field colour to the
 * ink, because the PS1 has no per-texel alpha; if the two disagree, every
 * wordmark sits in a visible rectangle of the wrong grey.
 *
 * Two colours, because drawRect() wants 0xBBGGRR and the fade maths wants the
 * channels separately.
 */
#define SCE_FIELD_R 168
#define SCE_FIELD_G 168
#define SCE_FIELD_B 168
#define SCE_FIELD   ((SCE_FIELD_B << 16) | (SCE_FIELD_G << 8) | SCE_FIELD_R)

#define SQ_CX   160
#define SQ_CY   117
#define SQW     256                  /* diamond half-width  = SQW / 4 = 64 */
#define SQH     268                  /* diamond half-height = SQH / 4 = 67 */
#define SQ_X    (SQ_CX - SQW / 2)
#define SQ_Y    (SQ_CY - SQH / 2)

#define PCT_X(p) (SQ_X + (SQW * (p)) / 100)
#define PCT_Y(p) (SQ_Y + (SQH * (p)) / 100)

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
	// Every vertex goes through the spin, so the Spin variant needs no
	// separate drawing code at all - it is the same quads, turned.
	int cx = SQ_CX, cy = SQ_CY;

	rotatePoint(cx, cy, &x0, &y0);
	rotatePoint(cx, cy, &x1, &y1);
	rotatePoint(cx, cy, &x2, &y2);
	rotatePoint(cx, cy, &x3, &y3);

	// Glass is the same fill drawn semi-transparently. Nothing else about
	// the shape or the gradient changes.
	//
	// White Ribbons is deliberately NOT in this list: the mark stays solid
	// there, exactly as in Classic. Making it see-through as well left it
	// washed out against the field behind it.
	bool blend = (introVariant == PS1_INTRO_GLASS);

	GPUDMAChain *chain = getCurrentChain(ctx);
	uint32_t *ptr = allocateGP0Packet(chain, 8);

	ptr[0] = c0 | gp0_shadedQuad(true, false, blend);
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
	/*
	 * U/V span `w`, not `w - 1`, and that is not a typo.
	 *
	 * The quad covers w pixels, x .. x+w-1. With the w-1 idiom used elsewhere
	 * in the project the GPU steps U by (w-1)/w per pixel, so the final pixel
	 * lands on u+w-2 and the texture's LAST COLUMN IS NEVER SAMPLED. Most
	 * sheets get away with it because their cells have a margin; these do not
	 * - the artwork runs edge to edge. That missing column is why the
	 * PlayStation wordmark rendered as "PlayStatior" with the final stem gone,
	 * and why the PS logo lost its right edge.
	 *
	 * Safe here because every one of these textures is at most 128 texels
	 * wide with a u base of 0, so u+w cannot reach the 256 wrap that the
	 * planet renderer in xmb_bg.c had to dodge. The padding column it may
	 * touch at the extreme right edge is transparent.
	 */
	ptr[2] = gp0_uv(tex->u, tex->v, tex->clut);
	ptr[3] = gp0_xy(x + w, y);
	ptr[4] = gp0_uv(tex->u + w, tex->v, tex->page);
	ptr[5] = gp0_xy(x, y + h);
	ptr[6] = gp0_uv(tex->u, tex->v + h, 0);
	ptr[7] = gp0_xy(x + w, y + h);
	ptr[8] = gp0_uv(tex->u + w, tex->v + h, 0);
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

/* ===================================================================== *
 * Variants
 *
 * These are ADJUSTMENTS to the original mark, not redesigns. The geometry,
 * the colours and the timeline are identical in all four - the first attempt
 * replaced the artwork with new shapes, which was the wrong idea. What
 * changes is only how the same quads are drawn:
 *
 *   Classic       as the original
 *   Glass         the same quads, blended, so the background shows through
 *   Spin          the same flat quads, rotated about the centre once
 *   White Ribbons the same solid mark, over white ribbons and sparkles
 * ===================================================================== */

void setPS1IntroVariant(int variant) {
	if (variant >= 0 && variant < PS1_INTRO_COUNT)
		introVariant = variant;
}

/*
 * WHY THE WORDMARKS LOOK SHARP NOW
 *
 * They used to be two-colour art: one transparent index and one ink index,
 * nothing in between. At 4bpp that throws away fourteen of the sixteen palette
 * entries and leaves every diagonal and every curve as a bare staircase, which
 * is exactly what the SONY "S" and "Y" and the "R" in COMPUTER looked like.
 * The real BIOS wordmarks are antialiased, and that - not resolution - is the
 * whole difference.
 *
 * So the art is now generated antialiased, by tools/make_intro_wordmarks.py
 * from the one-bit masters in assets/orig_intro_*.png. Coverage is quantised
 * to fifteen levels and baked into the CLUT as opaque colours ramping from
 * SCE_FIELD down to the ink, with index 0 left transparent so the sprite still
 * has no background box. Sixteen entries exactly - the most 4bpp holds, and
 * all convertImage.py will accept.
 *
 * Two consequences worth knowing:
 *
 *   - The ramp is baked against SCE_FIELD, because the PS1 has no per-texel
 *     alpha to blend with at draw time. Change SCE_FIELD and the tool's FIELD
 *     together or the wordmarks appear in boxes of the old colour. Over the
 *     White Ribbons variant the edge pixels are a touch flatter than their
 *     surroundings, which does not read at this size.
 *   - The texels are opaque (no STP bit), so the GPU ignores the blend flag on
 *     them and spriteFadeOnLight()'s repeated passes collapse into a single
 *     step. The wordmarks pop in rather than fading. That was already true of
 *     the old art and is left alone here.
 */
void initPS1Boot(RenderContext *ctx) {
	(void) ctx;

#if PS1_BOOT_TEXTURES

	uploadIndexedTexture(&sonyTex, introSonyTexture, introSonyPalette,
		INTRO_VRAM_X, SONY_VRAM_Y, SONY_CLUT_X, INTRO_CLUT_Y,
		SONY_W, SONY_H, GP0_COLOR_4BPP);

	uploadIndexedTexture(&computerTex, introComputerTexture, introComputerPalette,
		INTRO_VRAM_X, COMP_VRAM_Y, COMP_CLUT_X, INTRO_CLUT_Y,
		COMP_W, COMP_H, GP0_COLOR_4BPP);

	uploadIndexedTexture(&enterTex, introEnterTexture, introEnterPalette,
		INTRO_VRAM_X, ENTR_VRAM_Y, ENTR_CLUT_X, INTRO_CLUT_Y,
		ENTR_W, ENTR_H, GP0_COLOR_4BPP);

	uploadIndexedTexture(&tmTex, introTmTexture, introTmPalette,
		INTRO_VRAM_X, TM_VRAM_Y, TM_CLUT_X, INTRO_CLUT_Y,
		TM_W, TM_H, GP0_COLOR_4BPP);

	uploadIndexedTexture(&psLogoTex, introPsLogoTexture, introPsLogoPalette,
		INTRO_VRAM_X, PSLG_VRAM_Y, PSLG_CLUT_X, INTRO_CLUT_Y,
		PSLG_W, PSLG_H, GP0_COLOR_4BPP);

	uploadIndexedTexture(&psTextTex, introPsTextTexture, introPsTextPalette,
		INTRO_VRAM_X, PSTX_VRAM_Y, PSTX_CLUT_X, INTRO_CLUT_Y,
		PSTX_W, PSTX_H, GP0_COLOR_4BPP);
#endif
}

/* --- the SCE (white) screen ------------------------------------------- */

static void drawSceScreen(RenderContext *ctx, int frame) {
	/*
	 * Spin variant: one full turn over the same window the triangles
	 * animate in, easing to a stop so it lands square rather than
	 * snapping. Zero for every other variant, which makes rotatePoint()
	 * a no-op and leaves the original untouched.
	 */
	introSpin = 0;

	if (introVariant == PS1_INTRO_SPIN) {
		int k = ramp(frame, T_DIAMOND_IN, T_TRI_END);
		int ease = 256 - (((256 - k) * (256 - k)) >> 8);

		introSpin = ((256 - ease) * (ISIN_PI * 2)) / 256;
		introSpin &= (ISIN_PI * 2) - 1;
	}

	// The CSS gradient is horizontal: #E01705 at both edges, #DF9300 in the
	// middle. On a rhombus whose left and right corners sit at the edges and
	// whose top and bottom corners sit at the centre, that is exactly a
	// four-vertex Gouraud fill - no gradient texture needed.
	const uint32_t edge   = 0x0517e0;   /* 0xBBGGRR of #E01705 */
	const uint32_t centre = 0x0093df;   /* 0xBBGGRR of #DF9300 */

	int cx = SQ_CX;
	int cy = SQ_CY;
	int rx = SQW / 4;                   /* 50% box -> 25% half-extent */
	int ry = SQH / 4;

	int level = ramp(frame, T_DIAMOND_IN, T_DIAMOND_IN_END);

	if (level > 0) {
		// Fading a solid shape in on the flat field: interpolate its colour
		// from the field toward the real one, which is smooth and costs
		// nothing.
		uint32_t e = edge, c = centre;

		if (level < 256) {
			int k = level;
			uint32_t er = ((edge        & 0xff) * k + SCE_FIELD_R * (256 - k)) >> 8;
			uint32_t eg = (((edge >> 8)  & 0xff) * k + SCE_FIELD_G * (256 - k)) >> 8;
			uint32_t eb = (((edge >> 16) & 0xff) * k + SCE_FIELD_B * (256 - k)) >> 8;
			uint32_t cr = ((centre        & 0xff) * k + SCE_FIELD_R * (256 - k)) >> 8;
			uint32_t cg = (((centre >> 8)  & 0xff) * k + SCE_FIELD_G * (256 - k)) >> 8;
			uint32_t cb = (((centre >> 16) & 0xff) * k + SCE_FIELD_B * (256 - k)) >> 8;
			e = (eb << 16) | (eg << 8) | er;
			c = (cb << 16) | (cg << 8) | cr;
		}

		// TL, TR, BL, BR order: top and bottom points are the centre colour,
		// left and right points the edge colour.
		gouraudQuad(ctx,
			cx - rx, cy,      e,
			cx,      cy - ry, c,
			cx,      cy + ry, c,
			cx + rx, cy,      e);
	}

	/*
	 * The two triangles.
	 *
	 * The first version of this had them meeting at the centre and forming
	 * a second diamond, which is not what the real mark does at all. Read
	 * off the source properly this time:
	 *
	 *   Both start as 25%x50% boxes covering the diamond's bounding box -
	 *   triangle 1 the left half, triangle 2 the right - and shrink to
	 *   10%x20% while moving to fixed end positions taken straight from
	 *   startup.js:
	 *
	 *     t1  left 50%-9%  = 41%      top 50%-19% = 31%
	 *     t2  left 50%-1%  = 49%      top 50%-1%  = 49%
	 *
	 *   Triangle 1 is clip-path polygon(0% 50%, 100% 100%, 100% 0): a point
	 *   on the LEFT with a full-height edge on the right. Triangle 2 is the
	 *   mirror, pointing RIGHT. They end up offset from each other - one
	 *   above centre, one below - which is what cuts the two notches into
	 *   the diamond that make the SCE mark, rather than filling it in.
	 *
	 * All coordinates are percentages of the reference square, because the
	 * triangles are positioned against the container, not the diamond.
	 */
	if (frame >= T_TRI_START) {
		int k = ramp(frame, T_TRI_START, T_TRI_END);

		/* Sizes: 25%x50% -> 10%x20% of the reference box. */
		int w = ((25 + ((10 - 25) * k) / 256) * SQW) / 100;
		int h = ((50 + ((20 - 50) * k) / 256) * SQH) / 100;

		/* Top-left corners, in percent * 100 to keep the interpolation
		 * honest with integer maths. */
		int t1x = ((2500 + ((4100 - 2500) * k) / 256) * SQW) / 10000;
		int t1y = ((2500 + ((3100 - 2500) * k) / 256) * SQH) / 10000;
		int t2x = ((5000 + ((4900 - 5000) * k) / 256) * SQW) / 10000;
		int t2y = ((2500 + ((4900 - 2500) * k) / 256) * SQH) / 10000;

		t1x += SQ_X; t2x += SQ_X;
		t1y += SQ_Y; t2y += SQ_Y;

		// Triangle 1: apex on the left (red), full-height edge on the
		// right (amber). Fourth vertex repeats the third so the quad
		// degenerates into a triangle.
		gouraudQuad(ctx,
			t1x,     t1y + h / 2, edge,
			t1x + w, t1y,         centre,
			t1x + w, t1y + h,     centre,
			t1x + w, t1y + h,     centre);

		// Triangle 2: mirrored - apex on the right, edge on the left.
		gouraudQuad(ctx,
			t2x + w, t2y + h / 2, edge,
			t2x,     t2y,         centre,
			t2x,     t2y + h,     centre,
			t2x,     t2y + h,     centre);
	}

	/* SONY wordmark and the COMPUTER / ENTERTAINMENT block. */
	int textLevel = ramp(frame, T_SCE_TEXT, T_SCE_TEXT_END);

	if (frame >= T_SCE_OUT)
		textLevel = 256 - ramp(frame, T_SCE_OUT, T_SCE_OUT_END) ;

	if (textLevel > 0) {
#if PS1_BOOT_TEXTURES
		// Positions below are measured off the real BIOS screens, as
		// percentages of the 4:3 frame, rather than guessed from the CSS.
		spriteFadeOnLight(ctx, &sonyTex,
			(ctx->screenWidth - SONY_DW) / 2, 21, SONY_DW, SONY_DH, textLevel);

		spriteFadeOnLight(ctx, &computerTex,
			(ctx->screenWidth - COMP_DW) / 2, 194, COMP_DW, COMP_DH, textLevel);

		spriteFadeOnLight(ctx, &enterTex,
			(ctx->screenWidth - ENTR_DW) / 2, 207, ENTR_DW, ENTR_DH, textLevel);
#endif

		// The trademark mark. Sits just off the diamond's bottom-right
		// corner, as it does on the real screen.
		spriteFadeOnLight(ctx, &tmTex, cx + 8, 180, TM_DW, TM_DH, textLevel);
	}
}

/* --- the PlayStation (black) screen -----------------------------------
 *
 * THE 3D LOGO'S START AND END POSE.
 *
 * The animation is a straight interpolation between the two blocks below, so
 * these twelve numbers ARE the animation - there is nothing else to it. The
 * pose tool on the boot menu (entry 5) edits exactly these, live, and prints
 * them back in this form; read them off the screen and paste them here.
 *
 * Angles are degrees, offsets from the BIOS resting pose baked into the model.
 *
 *   YAW    turntable, about the vertical Y axis
 *   PITCH  flip, about the horizontal X axis
 *   ROLL   spin in the screen plane, about the depth Z axis
 *   CAM_Z  camera distance; smaller is bigger. The posed model is 326 units
 *          across and setupCosmosGTE() sets H = min(w,h)/2 = 120 at 320x240,
 *          so projected width is about 120 * 326 / camZ - 430 gives roughly
 *          110 pixels, against the ~100 the BIOS screen shows.
 *   POS    where the model's centre lands on screen, in pixels.
 */
#define DEG(d) (((d) * 4096) / 360)

/*
 * Which logo model the sequence uses: 0 = Sketchfab relief, 1 = system BIOS,
 * 2 = newly supplied full-resolution BIOS GLB. The pose tool cycles all three
 * live for hardware comparison; this is just the value it starts from.
 */
#define PS_LOGO_MODEL 2       /* finalized high-resolution C model */
#define PS_LOGO_TUNE_MODEL 2  /* open the pose/effect tool directly on C */
#define PS_LOGO_SHADE 0   /* finalized hardware-approved ALL MEDIUM bake */
#define PS_LOGO_EFFECT 28  /* R2 29/29: finalized COSMOS IMPACTS sequence */
#define PS_LOGO_ANIM   0

/* Dialled in on hardware with the pose tool - do not "tidy" these. */
#define PS_LOGO_START_YAW    148      /* tumbling in, from far away       */
#define PS_LOGO_START_PITCH  171
#define PS_LOGO_START_ROLL   103
#define PS_LOGO_START_CAM_Z 2320      /* distant, so it flies toward us   */
#define PS_LOGO_START_X      152
#define PS_LOGO_START_Y        7

#define PS_LOGO_END_YAW       21
#define PS_LOGO_END_PITCH      6
#define PS_LOGO_END_ROLL       4
#define PS_LOGO_END_CAM_Z    680
#define PS_LOGO_END_X        163
#define PS_LOGO_END_Y         77

/*
 * Frames the move takes, counted from T_PS_LOGO. Deliberately longer than the
 * brightness fade (T_PS_LOGO..T_PS_LOGO_END, 27 frames): the logo is fully lit
 * well before it stops moving, which is what makes the arrival read as a
 * settle rather than a fade.
 */
#define PS_LOGO_MOVE_FRAMES 120

/* Interpolate one value from its start to its end over ease (0..256). */
static int poseLerp(int from, int to, int ease) {
	return from + ((to - from) * ease) / 256;
}

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

	if (logoLevel > 0) {
#if PS1_BOOT_LOGO_3D
		/*
		 * The real model, GTE-transformed, rather than a flat sprite.
		 *
		 * It moves from the START pose to the END pose over
		 * PS_LOGO_MOVE_FRAMES and holds there for the rest of the screen.
		 * The fade is the same ramp as before, applied to the flat face
		 * colours instead of to a texture tint.
		 *
		 * The ease is cubic rather than the quadratic used for the SCE spin:
		 * over two seconds a quadratic still arrives visibly quickly, where
		 * this coasts most of the way and then settles.
		 */
		int k    = ramp(frame, T_PS_LOGO, T_PS_LOGO + PS_LOGO_MOVE_FRAMES);
		int inv  = 256 - k;
		int ease = 256 - ((((inv * inv) >> 8) * inv) >> 8);   /* 0 -> 256 */

		xmbDrawIntroPSLogo(ctx, PS_LOGO_MODEL,
			poseLerp(PS_LOGO_START_X,     PS_LOGO_END_X,     ease),
			poseLerp(PS_LOGO_START_Y,     PS_LOGO_END_Y,     ease),
			poseLerp(PS_LOGO_START_CAM_Z, PS_LOGO_END_CAM_Z, ease),
			DEG(poseLerp(PS_LOGO_START_YAW,   PS_LOGO_END_YAW,   ease)),
			DEG(poseLerp(PS_LOGO_START_PITCH, PS_LOGO_END_PITCH, ease)),
			DEG(poseLerp(PS_LOGO_START_ROLL,  PS_LOGO_END_ROLL,  ease)),
			clampi(logoLevel, 0, 256), PS_LOGO_SHADE,
			PS_LOGO_EFFECT, PS_LOGO_ANIM, (uint32_t) frame,
			(frame >= T_PS_LOGO + PS_LOGO_MOVE_FRAMES)
				? (uint32_t) (frame - (T_PS_LOGO + PS_LOGO_MOVE_FRAMES))
				: 0);
#elif PS1_BOOT_TEXTURES
		// The old flat sprite. Bright artwork on black: scaling the vertex
		// colour gives a genuinely smooth fade, because the GPU modulates
		// every texel by it.
		uint32_t g = (uint32_t) clampi((logoLevel * 128) / 256, 0, 128);
		uint32_t tint = (g << 16) | (g << 8) | g;

		sprite(ctx, &psLogoTex,
			(ctx->screenWidth - PSLG_DW) / 2, 26, PSLG_DW, PSLG_DH, tint, false);
#endif
	}

	if (textLevel > 0) {
#if PS1_BOOT_TEXTURES
		uint32_t g = (uint32_t) clampi((textLevel * 128) / 256, 0, 128);
		uint32_t tint = (g << 16) | (g << 8) | g;

		sprite(ctx, &psTextTex,
			(ctx->screenWidth - PSTX_DW) / 2, 138, PSTX_DW, PSTX_DH, tint, false);
#endif
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
				170 + i * 11, col, lines[i]);
		}
	}
}

/* --- the pose tool ----------------------------------------------------
 *
 * A live editor for the twelve numbers above, so the start and end poses can
 * be dialled in on the console and read straight off the screen instead of
 * being guessed from a screenshot and rebuilt each time.
 *
 * The model is drawn at whichever pose is selected, full brightness, on black.
 * Everything is in degrees and pixels - the same units the #defines use - so
 * what the readout shows is what gets pasted back.
 *
 * It runs from the boot menu, before initPS1Boot(), which is fine: the logo is
 * geometry, not a texture, so it needs nothing uploaded.
 */

/* Field order on screen; also the order of the values[] array below. */
enum {
	POSE_YAW, POSE_PITCH, POSE_ROLL, POSE_CAM_Z, POSE_X, POSE_Y,
	POSE_FIELD_COUNT
};

static const char *const poseFieldNames[POSE_FIELD_COUNT] = {
	"YAW   (Y turntable)",
	"PITCH (X flip)",
	"ROLL  (Z screen spin)",
	"CAM Z (smaller=bigger)",
	"POS X",
	"POS Y",
};

/* Per-field limits and the step a single D-pad press moves. */
static const int poseMin[POSE_FIELD_COUNT]  = { -180, -180, -180,  60,  -64,  -64 };
static const int poseMax[POSE_FIELD_COUNT]  = {  180,  180,  180, 4000, 384,  304 };
static const int poseStep[POSE_FIELD_COUNT] = {    1,    1,    1,   10,    1,    1 };

void runPSLogoPoseTool(RenderContext *ctx) {
	int pose[2][POSE_FIELD_COUNT] = {
		{ PS_LOGO_START_YAW, PS_LOGO_START_PITCH, PS_LOGO_START_ROLL,
		  PS_LOGO_START_CAM_Z, PS_LOGO_START_X, PS_LOGO_START_Y },
		{ PS_LOGO_END_YAW, PS_LOGO_END_PITCH, PS_LOGO_END_ROLL,
		  PS_LOGO_END_CAM_Z, PS_LOGO_END_X, PS_LOGO_END_Y },
	};

	int which = 0;          /* 0 = START, 1 = END */
	int sel   = 0;
	int play  = -1;         /* frame of the preview, or -1 when not playing */
	int model = PS_LOGO_TUNE_MODEL;
	int shade = PS_LOGO_SHADE;
	int effect = PS_LOGO_EFFECT;
	int anim = PS_LOGO_ANIM;
	uint32_t fxFrame = 0;
	uint32_t settledFrames = 0;
	bool sawRelease = false;
	uint16_t last = 0;

	for (;;) {
		uint16_t buttons = pollController(0) | pollController(1);
		uint16_t pressed = buttons & ~last;

		last = buttons;

		if (!buttons)
			sawRelease = true;

		if (sawRelease) {
			if (pressed & PAD_BTN_CIRCLE)
				break;
			if (pressed & PAD_BTN_TRIANGLE)
				which ^= 1;
			if (pressed & PAD_BTN_SQUARE)
				play = 0;
			if (pressed & PAD_BTN_L1)
				model = (model + 1) % XMB_INTRO_LOGO_COUNT;
			int shadeCount = xmbIntroPSLogoShadeCount(model);
			if ((pressed & PAD_BTN_R1) && shadeCount > 1)
				shade = (shade + 1) % shadeCount;
			int effectCount = xmbIntroPSLogoEffectCount(model);
			if ((pressed & PAD_BTN_R2) && effectCount > 1) {
				effect = (effect + 1) % effectCount;
				/* Selecting the absorption effect while already viewing END
				 * must still give its full one-second calm period. */
				settledFrames = 0;
			}
			int animCount = xmbIntroPSLogoAnimCount(model);
			if ((pressed & PAD_BTN_L2) && animCount > 1)
				anim = (anim + 1) % animCount;
			if (pressed & PAD_BTN_UP)
				sel = (sel + POSE_FIELD_COUNT - 1) % POSE_FIELD_COUNT;
			if (pressed & PAD_BTN_DOWN)
				sel = (sel + 1) % POSE_FIELD_COUNT;

			// Held rather than pressed, so A/B values can be run up quickly.
			// C uses all four shoulder buttons for model/shadow/effect/animation,
			// so its pose fields intentionally stay at precise single steps.
			uint16_t coarse = (model == 2) ? 0 : PAD_BTN_R1;
			int step = poseStep[sel] * ((buttons & coarse) ? 10 : 1);

			if (buttons & PAD_BTN_LEFT)
				pose[which][sel] -= step;
			if (buttons & PAD_BTN_RIGHT)
				pose[which][sel] += step;
			if (buttons & (PAD_BTN_LEFT | PAD_BTN_RIGHT))
				settledFrames = 0;

			pose[which][sel] = clampi(pose[which][sel],
				poseMin[sel], poseMax[sel]);
		}

		/*
		 * What to draw: the selected pose while editing, or the interpolated
		 * one while the preview runs. The preview uses the same cubic ease
		 * and the same frame count the real sequence does, so it is not an
		 * approximation of the animation - it is the animation.
		 */
		int shown[POSE_FIELD_COUNT];

		bool logoSettled;
		if (play >= 0) {
			int previewFrame = play;
			int k    = ramp(play, 0, PS_LOGO_MOVE_FRAMES);
			int inv  = 256 - k;
			int ease = 256 - ((((inv * inv) >> 8) * inv) >> 8);

			for (int i = 0; i < POSE_FIELD_COUNT; i++)
				shown[i] = poseLerp(pose[0][i], pose[1][i], ease);

			logoSettled = previewFrame >= PS_LOGO_MOVE_FRAMES;
			/* Hold END long enough to see the one-second wait, an incoming
			 * comet, its absorption and the resulting firework. */
			if (++play > PS_LOGO_MOVE_FRAMES + 240)
				play = -1;
		} else {
			for (int i = 0; i < POSE_FIELD_COUNT; i++)
				shown[i] = pose[which][i];
			logoSettled = which == 1;
		}

		if (logoSettled)
			settledFrames++;
		else
			settledFrames = 0;

		beginFrame(ctx);
		drawRect(ctx, 0, 0, ctx->screenWidth, ctx->screenHeight,
			0x000000, false);

		xmbDrawIntroPSLogo(ctx, model,
			shown[POSE_X], shown[POSE_Y], shown[POSE_CAM_Z],
			DEG(shown[POSE_YAW]), DEG(shown[POSE_PITCH]),
			DEG(shown[POSE_ROLL]), 256, shade,
			effect, anim, fxFrame++, settledFrames);

		printString(ctx, 8, 8, 0xffffff, "PS LOGO POSE");
		printString(ctx, 104, 8, 0x40c0ff,
			(play >= 0) ? "PREVIEW" : (which ? "END" : "START"));
		printString(ctx, 168, 8, 0xc0c060, xmbIntroLogoNames[model]);

		int shadeCount = xmbIntroPSLogoShadeCount(model);
		if (shadeCount > 1) {
			char line[48];
			snprintf(line, sizeof(line), "R1 SHADOW %d/%d  %s",
				shade + 1, shadeCount,
				xmbIntroPSLogoShadeName(model, shade));
			printString(ctx, 8, 90, 0xffa040, line);
		}

		int effectCount = xmbIntroPSLogoEffectCount(model);
		if (effectCount > 1) {
			char line[48];
			snprintf(line, sizeof(line), "R2 EFFECT %d/%d  %s",
				effect + 1, effectCount,
				xmbIntroPSLogoEffectName(model, effect));
			printString(ctx, 8, 100, 0x70c0ff, line);
		}

		int animCount = xmbIntroPSLogoAnimCount(model);
		if (animCount > 1) {
			char line[48];
			snprintf(line, sizeof(line), "L2 ANIM %d/%d  %s",
				anim + 1, animCount,
				xmbIntroPSLogoAnimName(model, anim));
			printString(ctx, 8, 110, 0xc080ff, line);
		}

		for (int i = 0; i < POSE_FIELD_COUNT; i++) {
			char line[48];
			int y = 24 + i * 10;

			snprintf(line, sizeof(line), "%c %-22s %5d",
				(i == sel) ? '>' : ' ', poseFieldNames[i], pose[which][i]);
			printString(ctx, 8, y, (i == sel) ? 0xffffff : 0x909090, line);
		}

		printString(ctx, 8, 210, 0x707070,
			CH_PS1_TRIANGLE_BUTTON " start/end   "
			CH_PS1_SQUARE_BUTTON " preview   "
			CH_PS1_CIRCLE_BUTTON " back");
		printString(ctx, 8, 222, 0x707070,
			(model == 2)
				? "D-PAD adjust L1 model R2 FX L2 anim"
				: (shadeCount > 1)
				? "D-PAD adjust L1 model R1 shade R2 FX L2 anim"
				: "D-PAD pick/adjust   R1 x10   L1 model");

		endFrame(ctx);
	}

	while (pollController(0) | pollController(1))
		;
}

/*
 * Variant picker.
 *
 * Deliberately plain: a dark screen, a few lines, D-pad and X. It runs before
 * any of the boot artwork is uploaded, so it cannot depend on it.
 *
 * The last entry is not a variant - it opens the pose tool and comes back
 * here, so it is handled inside the loop rather than returned.
 */
#define INTRO_MENU_POSE_TOOL PS1_INTRO_COUNT
#define INTRO_MENU_COUNT     (PS1_INTRO_COUNT + 1)

int chooseIntroVariant(RenderContext *ctx) {
	static const char *const names[INTRO_MENU_COUNT] = {
		"1  Classic        flat gradient, as original",
		"2  Glass          same logo, see-through",
		"3  Spin           same logo, spun once",
		"4  White Ribbons  solid logo, white ribbons",
		"5  Logo pose tool  edit the 3D start/end",
	};

	int sel = 0;
	bool sawRelease = false;

	for (;;) {
		uint16_t buttons = pollController(0) | pollController(1);
		static uint16_t last = 0;
		uint16_t pressed = buttons & ~last;

		last = buttons;

		if (!buttons)
			sawRelease = true;

		if (sawRelease) {
			if (pressed & PAD_BTN_UP)
				sel = (sel + INTRO_MENU_COUNT - 1) % INTRO_MENU_COUNT;
			if (pressed & PAD_BTN_DOWN)
				sel = (sel + 1) % INTRO_MENU_COUNT;
			if (pressed & (PAD_BTN_CROSS | PAD_BTN_START)) {
				if (sel != INTRO_MENU_POSE_TOOL)
					break;

				runPSLogoPoseTool(ctx);
				sawRelease = false;
				last = 0;
			}
		}

		beginFrame(ctx);
		drawRect(ctx, 0, 0, ctx->screenWidth, ctx->screenHeight,
			0x201008, false);

		printString(ctx, 16, 24, 0xffffff, "INTRO STYLE TEST");
		printString(ctx, 16, 40, 0x808080,
			"D-PAD choose      " CH_PS1_CROSS_BUTTON " start");

		for (int i = 0; i < INTRO_MENU_COUNT; i++) {
			printString(ctx, 24, 72 + i * 16,
				(i == sel) ? 0x1256e3 : 0xa0a0a0, names[i]);
		}

		endFrame(ctx);
	}

	while (pollController(0) | pollController(1))
		;

	return sel;
}

void runPS1Boot(RenderContext *ctx) {
	/*
	 * Skip handling is deliberately defensive.
	 *
	 * The first version broke out of the loop the moment any button bit was
	 * set, then span in an unbounded "wait for release" afterwards. A pad
	 * reporting a stuck bit - or a multitap, or an emulator mapping - would
	 * therefore skip the whole sequence on frame 0 and then hang forever
	 * waiting for a release that never comes: a black screen with the music
	 * still playing, which is exactly what that looks like.
	 *
	 * Now a skip has to be a genuine press: the pad must read clear first,
	 * and every wait is bounded.
	 */
	bool sawRelease = false;

	// The jingle runs the length of the sequence and borrows the BGM slot,
	// so the music has to be restored on every exit path below - including
	// a skip.
	playIntroJingle();

	for (int frame = 0; frame < T_END; frame++) {
		uint16_t buttons = pollController(0) | pollController(1);

		if (!buttons)
			sawRelease = true;
		else if (sawRelease)
			break;

		beginFrame(ctx);

		bool white = frame < T_TO_BLACK;

		drawRect(ctx, 0, 0, ctx->screenWidth, ctx->screenHeight,
			white ? SCE_FIELD : 0x000000, false);

		/*
		 * Variant 4 replaces the flat white field with the Parallax
		 * Ribbons theme's own ribbons and the PS5 Sparkle theme's own
		 * rising particles, both rendered in white - see
		 * xmbDrawWhiteRibbons(). It is called directly rather than through
		 * drawXMBBackground(), so the user's chosen theme index is left
		 * alone - the previous version of this had to save it, overwrite
		 * it and put it back.
		 */
		if (white && introVariant == PS1_INTRO_WHITE_RIBBONS)
			xmbDrawIntroRibbons(ctx, SCE_FIELD);

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

	restoreBGMAfterIntro();

	// Do not pass a held button straight to the menu - but never wait
	// indefinitely for that, for the reason above.
	for (int guard = 0; guard < 120; guard++) {
		if (!(pollController(0) | pollController(1)))
			break;

		beginFrame(ctx);
		drawRect(ctx, 0, 0, ctx->screenWidth, ctx->screenHeight,
			0x000000, false);
		endFrame(ctx);
	}
}
