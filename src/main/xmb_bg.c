/*
 * PSX-iTests - XMB-style wave background (drop-in for ps1-bare-metal)
 *
 * MIT / ISC, same as the rest of the project.
 *
 * How it works (all three styles share the same two tricks):
 *
 *   1. Motion is an integer sine LUT. trig.c's isin() takes an angle where a
 *      full circle == (1 << (ISIN_SHIFT + 2)) == 4096 units, and returns a
 *      value in the range [-4096, +4096] (peak == 1 << 12). So a pixel offset
 *      of amplitude A is simply:  (isin(angle) * A) >> 12.
 *
 *   2. Glow is real additive blending. We set the texture-page blend bits to
 *      GP0_BLEND_ADD once per layer, then draw blended Gouraud quads. Where
 *      the quads overlap, colours add together -> the bright XMB "light on
 *      light" look. Bottom vertices are black, so each strip fades to nothing
 *      (adding black == no change) instead of needing an alpha channel.
 *
 * Cost: a few dozen quads per frame, zero VRAM, zero CPU sine math beyond the
 * LUT. Draw order is back-to-front implicitly because everything here is
 * emitted before the menu.
 */

#include <stdint.h>
#include <stdio.h>
#include "common/gpu.h"
#include "main/font.h"
#include "main/renderer.h"
#include "main/trig.h"
#include "main/xmb_bg.h"
#include "main/model/ps_logo_model.h"
#include "main/model/ps_logo_bios.h"
#include "main/model/ps_logo_bios2.h"
#include "ps1/cop0.h"
#include "ps1/gte.h"
#include "ps1/gpucmd.h"

/* isin() peaks at 1<<12; a full circle is 4096 LUT units. */
#define WAVE_ONE   (1 << 12)
#define WAVE_FULL  (1 << (ISIN_SHIFT + 2))   /* 4096 */

static XMBBgStyle currentStyle   = XMB_BG_GOURAUD_WAVES;
static uint32_t   xmbFrame       = 0;

/* User-facing theme picker: names shown in the menu, and the style each one
 * maps to. Keep these two arrays in the same order, XMB_THEME_COUNT long. */
const char *const xmbThemeNames[XMB_THEME_COUNT] = {
	"Default - Customize",
	"Gouraud Waves + Sparkle",
	"Aurora",
	"Parallax Ribbons",
	"Space Cosmos",
	"Space Cosmos 3D++1",
	"Space Cosmos 3D++2",
	"PS5 Sparkle",
	"PS5 Spotlight",
	"Nebula 3",
	"PS4",
	"PS4 v2"
};
static const XMBBgStyle themeStyles[XMB_THEME_COUNT] = {
	XMB_BG_GOURAUD_PSP,
	XMB_BG_GOURAUD_SPARKLE,
	XMB_BG_AURORA,
	XMB_BG_PARALLAX,
	XMB_BG_COSMOS,
	XMB_BG_COSMOS_3D_PP,
	XMB_BG_COSMOS_3D_PP2,
	XMB_BG_PS5,
	XMB_BG_PS5_SPOTLIGHT,
	XMB_BG_NEBULA3,
	XMB_BG_PS4,
	XMB_BG_PS4_V2
};
uint8_t xmbThemeIndex = 0;

void xmbBgSetStyle(XMBBgStyle style) { currentStyle = style; }
XMBBgStyle xmbBgGetStyle(void)       { return currentStyle;  }

/* --- helpers ------------------------------------------------------------ */

/* Select the tpage blend mode (and dithering) for following primitives. */
static void setBlend(GPUDMAChain *chain, GP0BlendMode blend) {
	uint32_t *ptr = allocateGP0Packet(chain, 1);
	ptr[0] = gp0_setPage(
		gp0_page(0, 0, blend, GP0_COLOR_16BPP),
		true,   /* dither: smooths the gradients, matches GPU output */
		false
	);
}

/*
 * Emit one Gouraud quad. Vertex order matches the PS1 strip convention used
 * elsewhere in the project (TL, TR, BL, BR). Colours are packed 0x00BBGGRR,
 * same as gp0_rgb(). Caller sets the blend page beforehand.
 */
static void gouraudQuad(
	GPUDMAChain *chain,
	int x0, int y0, uint32_t c0,   /* top-left  */
	int x1, int y1, uint32_t c1,   /* top-right */
	int x2, int y2, uint32_t c2,   /* bottom-left  */
	int x3, int y3, uint32_t c3,   /* bottom-right */
	bool blend
) {
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

/* Scale each channel of a 0x00BBGGRR colour by num/256 (cheap shimmer). */
static uint32_t scaleColor(uint32_t c, int num) {
	int r = ( c        & 0xff) * num >> 8;
	int g = ((c >>  8) & 0xff) * num >> 8;
	int b = ((c >> 16) & 0xff) * num >> 8;
	if (r > 255) r = 255;
	if (g > 255) g = 255;
	if (b > 255) b = 255;
	return gp0_rgb(r, g, b);
}

/* Gouraud triangle (used for soft radial nebula glows via a fan). */
static void gouraudTri(
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

/* Flat (single-colour) quad. */
static void flatQuad(
	GPUDMAChain *chain,
	int x0, int y0, int x1, int y1,
	int x2, int y2, int x3, int y3,
	uint32_t color, bool blend
) {
	uint32_t *ptr = allocateGP0Packet(chain, 5);
	ptr[0] = color | gp0_shadedQuad(false, false, blend);
	ptr[1] = gp0_xy(x0, y0);
	ptr[2] = gp0_xy(x1, y1);
	ptr[3] = gp0_xy(x2, y2);
	ptr[4] = gp0_xy(x3, y3);
}

/* Gouraud line: bright head -> dim tail, for star trails. */
static void shadedLine(
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

/* Small filled point. */
static void point(
	GPUDMAChain *chain, int x, int y, int size, uint32_t color, bool blend
) {
	uint32_t *ptr = allocateGP0Packet(chain, 3);
	ptr[0] = color | gp0_rectangle(false, false, blend);
	ptr[1] = gp0_xy(x - size / 2, y - size / 2);
	ptr[2] = gp0_xy(size, size);
}

/* forward decl: soft radial glow (defined with the cosmos helpers below) */
static void nebulaBlob(GPUDMAChain *chain, int cx, int cy, int radius, uint32_t centre);

/* --- style 0: Gouraud waves (authentic XMB) ----------------------------- */

typedef struct {
	int baseY, amp, freq, speed, height, tilt, phase, segments;
} WaveLayer;

static void drawGouraudCore(RenderContext *ctx, GPUDMAChain *chain) {
	int w = ctx->screenWidth;
	int h = ctx->screenHeight;

	/* Base vertical wash: deep navy at the top fading to bright blue at the
	 * bottom, sampled from the reference wallpaper. Non-additive. */
	setBlend(chain, GP0_BLEND_SEMITRANS);
	{
		uint32_t top = gp0_rgb(  5,  15,  76); /* deep navy   */
		uint32_t bot = gp0_rgb( 43,  76, 218); /* bright blue */
		gouraudQuad(chain,
			0, 0, top,  w, 0, top,
			0, h, bot,  w, h, bot,
			false);
	}

	/* Three broad, soft light-blue bands centred low-middle. Each runs at a
	 * different frequency, speed and phase, and they overlap vertically, so
	 * they drift in and out of step and cross over one another - additive
	 * blending makes the crossings glow, giving the PS3 "intertwining" weave.
	 * All rise gently to the right (tilt) to sweep like the reference. */
	static const WaveLayer layers[] = {
		/* baseY amp freq speed height tilt  phase seg */
		{  126,  16,  26,   6,   60,   12,     0,  12 }, /* lead     */
		{  138,  20,  34,   9,   52,   16,   680,  12 }, /* mid      */
		{  132,  24,  20,   5,   64,   20,  1360,  12 }, /* trailing */
	};

	uint32_t crest[3];
	crest[0] = gp0_rgb(120, 180, 255); /* light blue  */
	crest[1] = gp0_rgb( 85, 150, 245); /* mid blue    */
	crest[2] = gp0_rgb(150, 205, 255); /* pale blue   */

	uint32_t black = gp0_rgb(0, 0, 0);

	setBlend(chain, GP0_BLEND_ADD);

	for (int L = 0; L < 3; L++) {
		const WaveLayer *lay = &layers[L];
		int step = w / lay->segments;

		for (int i = 0; i < lay->segments; i++) {
			int x0 = i * step;
			int x1 = (i == lay->segments - 1) ? w : (i + 1) * step;

			int a0 = (lay->phase + i       * lay->freq + xmbFrame * lay->speed)
				& (WAVE_FULL - 1);
			int a1 = (lay->phase + (i + 1) * lay->freq + xmbFrame * lay->speed)
				& (WAVE_FULL - 1);

			/* undulation + a gentle rightward rise (tilt) */
			int y0 = lay->baseY + (isin(a0) * lay->amp >> 12) - (x0 * lay->tilt >> 8);
			int y1 = lay->baseY + (isin(a1) * lay->amp >> 12) - (x1 * lay->tilt >> 8);

			/* mild horizontal shimmer: 65%..100% */
			int b0 = 166 + (isin((a0 * 2) & (WAVE_FULL - 1)) * 89 >> 12);
			int b1 = 166 + (isin((a1 * 2) & (WAVE_FULL - 1)) * 89 >> 12);
			uint32_t c0 = scaleColor(crest[L], b0);
			uint32_t c1 = scaleColor(crest[L], b1);

			/* Light crest, fading down over `height` px to nothing. */
			gouraudQuad(chain,
				x0, y0,               c0,    x1, y1,               c1,
				x0, y0 + lay->height, black, x1, y1 + lay->height, black,
				true);
		}
	}

	/* Leave the GPU back in a normal blend mode for the menu. */
	setBlend(chain, GP0_BLEND_SEMITRANS);
}

static void drawGouraudWaves(RenderContext *ctx, GPUDMAChain *chain) {
	drawGouraudCore(ctx, chain);
}

/* Gouraud waves with a light scattering of twinkling sparkles drifting over
 * the wave band, à la the PS3 XMB. Kept subtle - just a handful of dots. */
static void drawGouraudSparkle(RenderContext *ctx, GPUDMAChain *chain) {
	int w = ctx->screenWidth;
	uint32_t t = xmbFrame;

	drawGouraudCore(ctx, chain);

	setBlend(chain, GP0_BLEND_ADD);
	const int N = 26;
	for (int i = 0; i < N; i++) {
		/* Some sparkles drift smoothly right-to-left; the rest just twinkle
		 * in place with a little sway. */
		int drift = (i & 1) ? ((int) t * (1 + (i % 3))) / 2 : 0;
		int bx = ((i * 97) - drift) % w;
		if (bx < 0) bx += w;
		int x = bx + (isin((t * 2 + i * 150) & (WAVE_FULL - 1)) * 5 >> 12);
		int y = 92 + ((i * 47) % 96)
			+ (icos((t * 3 + i * 90) & (WAVE_FULL - 1)) * 5 >> 12);

		int tw = 110 + (isin((t * (4 + (i % 5)) + i * 300) & (WAVE_FULL - 1))
			* 145 >> 12);
		if (tw < 0) tw = 0;

		if (i % 3 == 0) {
			/* blurry, roughly double-size soft glow */
			nebulaBlob(chain, x, y, 5 + (i % 2) * 2,
				scaleColor(gp0_rgb(150, 190, 255), tw * 3 / 4));
		} else {
			/* crisp sparkle */
			point(chain, x, y, (i % 4 == 0) ? 2 : 1,
				scaleColor(gp0_rgb(200, 225, 255), tw), true);
		}
	}
	setBlend(chain, GP0_BLEND_SEMITRANS);
}

typedef struct {
	int      baseY, amp, freq, speed, height;
	uint32_t color;
} Ribbon;

static void drawParallax(RenderContext *ctx, GPUDMAChain *chain) {
	int w = ctx->screenWidth;
	int h = ctx->screenHeight;

	setBlend(chain, GP0_BLEND_SEMITRANS);
	{
		uint32_t top = gp0_rgb(  6,  12,  40); /* dark blue      */
		uint32_t bot = gp0_rgb( 18,  30,  78); /* deeper mid-blue*/
		gouraudQuad(chain,
			0, 0, top,  w, 0, top,
			0, h, bot,  w, h, bot,
			false);
	}

	Ribbon bands[4];
	bands[0] = (Ribbon){ 120, 30, 40,  8, 70, gp0_rgb( 60, 110, 235) }; /* blue   */
	bands[1] = (Ribbon){ 150, 44, 28, 12, 80, gp0_rgb( 30, 200, 210) }; /* cyan   */
	bands[2] = (Ribbon){ 180, 26, 60, 17, 60, gp0_rgb(160,  70, 220) }; /* violet */
	bands[3] = (Ribbon){ 205, 38, 34, 22, 70, gp0_rgb( 40, 160, 250) }; /* azure  */

	const int SEG = 16;
	int step = w / SEG;
	uint32_t black = gp0_rgb(0, 0, 0);

	setBlend(chain, GP0_BLEND_ADD);

	for (int r = 0; r < 4; r++) {
		Ribbon *B = &bands[r];
		for (int i = 0; i < SEG; i++) {
			int x0 = i * step;
			int x1 = (i == SEG - 1) ? w : (i + 1) * step;

			int a0 = (i       * B->freq + xmbFrame * B->speed) & (WAVE_FULL - 1);
			int a1 = ((i + 1) * B->freq + xmbFrame * B->speed) & (WAVE_FULL - 1);
			int y0 = B->baseY + (isin(a0) * B->amp >> 12);
			int y1 = B->baseY + (isin(a1) * B->amp >> 12);

			gouraudQuad(chain,
				x0, y0,          B->color, x1, y1,          B->color,
				x0, y0 + B->height, black,  x1, y1 + B->height, black,
				true);
		}
	}

	setBlend(chain, GP0_BLEND_SEMITRANS);
}

/* --- white ribbon field, for the boot intro ----------------------------- */

/*
 * The Parallax Ribbons theme and the PS5 Sparkle theme's particles, both in
 * white, over the boot sequence's SCE field. `field` is that field's colour
 * (0x00BBGGRR) and must be the intro's SCE_FIELD, or the ribbons will sit on a
 * different grey than the wordmarks were antialiased against.
 *
 * The ribbons are the SAME four bands as drawParallax() above - same baseY,
 * amplitude, frequency, speed, height and segment count - and the sparkles are
 * the SAME thirty particles as drawPS5Sparkle(). Nothing about the motion is
 * re-derived; only the colours change, which is the only honest way to say
 * "the Parallax animation, in white".
 *
 * The blend mode is additive, exactly as in both originals. That works here
 * for the same reason it works there: the field is a mid grey, so there is
 * headroom above it, and adding light makes each ribbon a WHITER shade of the
 * background rather than a darker one. (An earlier version of this drew on a
 * pure white field, where additive does nothing - white is already saturated -
 * and had to subtract instead, which made everything greyer than the
 * background. Matching the BIOS grey is what let it flip back.)
 */
void xmbDrawIntroRibbons(RenderContext *ctx, uint32_t field) {
	GPUDMAChain *chain = getCurrentChain(ctx);
	int w = ctx->screenWidth;
	int h = ctx->screenHeight;

	if (isBackgroundScrollEnabled())
		xmbFrame++;

	uint32_t t = xmbFrame;

	/* The field itself, flat: the wordmarks are baked against exactly this. */
	setBlend(chain, GP0_BLEND_SEMITRANS);
	gouraudQuad(chain,
		0, 0, field,  w, 0, field,
		0, h, field,  w, h, field,
		false);

	/*
	 * The four bands, byte-for-byte drawParallax()'s geometry. Only `color`
	 * differs: it is now how much light each ribbon ADDS, so a larger number
	 * is a whiter ribbon. The four values keep the original's front-to-back
	 * separation - the two foreground bands are the strongest.
	 */
	Ribbon bands[4];
	bands[0] = (Ribbon){ 120, 30, 40,  8, 70, gp0_rgb(38, 38, 36) };
	bands[1] = (Ribbon){ 150, 44, 28, 12, 80, gp0_rgb(62, 62, 60) };
	bands[2] = (Ribbon){ 180, 26, 60, 17, 60, gp0_rgb(26, 26, 25) };
	bands[3] = (Ribbon){ 205, 38, 34, 22, 70, gp0_rgb(50, 50, 48) };

	const int SEG = 16;
	int step = w / SEG;
	uint32_t none = gp0_rgb(0, 0, 0);   /* add nothing = background */

	setBlend(chain, GP0_BLEND_ADD);

	for (int r = 0; r < 4; r++) {
		Ribbon *B = &bands[r];

		for (int i = 0; i < SEG; i++) {
			int x0 = i * step;
			int x1 = (i == SEG - 1) ? w : (i + 1) * step;

			int a0 = (i       * B->freq + t * B->speed) & (WAVE_FULL - 1);
			int a1 = ((i + 1) * B->freq + t * B->speed) & (WAVE_FULL - 1);
			int y0 = B->baseY + (isin(a0) * B->amp >> 12);
			int y1 = B->baseY + (isin(a1) * B->amp >> 12);

			gouraudQuad(chain,
				x0, y0,             B->color, x1, y1,             B->color,
				x0, y0 + B->height, none,     x1, y1 + B->height, none,
				true);
		}
	}

	/*
	 * drawPS5Sparkle()'s particles: same count, same horizontal spread, same
	 * upward drift and same twinkle, in white rather than warm gold, so they
	 * read as bright flecks rising through the grey.
	 */
	{
		const int N = 30;
		int period = h + 40;

		for (int i = 0; i < N; i++) {
			int bx = (i * 79) % w
				+ (isin((t * 2 + i * 130) & (WAVE_FULL - 1)) * 4 >> 12);
			int drift = ((int) t * (1 + (i % 3)) + i * 47) % period;
			int y  = h - drift;

			int tw = 120 + (isin((t * (4 + (i % 5)) + i * 211) & (WAVE_FULL - 1))
				* 135 >> 12);
			if (tw < 0) tw = 0;

			uint32_t c = scaleColor(gp0_rgb(96, 96, 92), tw);
			int size = (i % 7 == 0) ? 3 : ((i % 3 == 0) ? 2 : 1);

			point(chain, bx, y, size, c, true);
		}
	}

	setBlend(chain, GP0_BLEND_SEMITRANS);
}


/* --- colour palettes for the PSP wave themes ---------------------------- */

const char *const xmbPaletteNames[XMB_PALETTE_COUNT] = {
	/* 12 basic colours (Default is the original blue) */
	"Default", "Crimson", "Ember", "Sunburst", "Lime", "Emerald",
	"Spring", "Aqua", "Azure", "Indigo", "Magenta", "Rose",
	/* 8 flat palette sets (from the reference swatches) */
	"Sunset Berry", "Deep Teal", "Hot Magenta", "Golden Hour",
	"Meadow", "Punch", "Autumn", "Rainforest"
};
uint8_t xmbPaletteIndex = 0;

/*
 * Each palette drives the whole scene: the background gradient, the two wave
 * layers and the sparkles. Raw RGB triples (not gp0_rgb()) because these are
 * static initialisers. "Default" reproduces the values sampled from the PSP
 * model's own texture, so palette 0 is byte-for-byte the original look.
 *
 * The last entry is built from the supplied reference swatch image:
 * coral (204,105,107), peach (234,173,161), mint (190,210,196),
 * teal (73,145,157) and navy (32,45,83).
 */
typedef struct {
	uint8_t bgTop[3], bgBot[3];
	uint8_t crestA[3], crestB[3];
	uint8_t sparkGlow[3], sparkDot[3];
} XMBPalette;

static const XMBPalette xmbPalettes[XMB_PALETTE_COUNT] = {
	/* Default - sampled from diss_00.png (mean RGB 17,17,45); kept
	 * byte-for-byte so the original look is still selectable. */
	{ {  8,  8, 26}, { 17, 17, 45}, {210,225,255}, {120,150,235},
	  {160,190,255}, {220,235,255} },
	/* --- 11 further basic hues around the colour wheel --------------- */
	/* Crimson */
	{ { 30,  8,  8}, { 55, 15, 15}, {246,156,156}, {235, 35, 35},
	  {246,156,156}, {251,215,215} },
	/* Ember */
	{ { 30, 18,  8}, { 55, 34, 15}, {246,198,156}, {235,128, 35},
	  {246,198,156}, {251,232,215} },
	/* Sunburst */
	{ { 30, 25,  8}, { 55, 47, 15}, {246,228,156}, {235,195, 35},
	  {246,228,156}, {251,244,215} },
	/* Lime */
	{ { 22, 30,  8}, { 42, 55, 15}, {216,246,156}, {168,235, 35},
	  {216,246,156}, {239,251,215} },
	/* Emerald */
	{ {  8, 30, 11}, { 15, 55, 22}, {156,246,171}, { 35,235, 68},
	  {156,246,171}, {215,251,221} },
	/* Spring */
	{ {  8, 30, 22}, { 15, 55, 42}, {156,246,216}, { 35,235,168},
	  {156,246,216}, {215,251,239} },
	/* Aqua */
	{ {  8, 28, 30}, { 15, 52, 55}, {156,238,246}, { 35,218,235},
	  {156,238,246}, {215,248,251} },
	/* Azure */
	{ {  8, 21, 30}, { 15, 38, 55}, {156,209,246}, { 35,152,235},
	  {156,209,246}, {215,236,251} },
	/* Indigo */
	{ { 15,  8, 30}, { 28, 15, 55}, {186,156,246}, {102, 35,235},
	  {186,156,246}, {227,215,251} },
	/* Magenta */
	{ { 30,  8, 30}, { 55, 15, 55}, {246,156,246}, {235, 35,235},
	  {246,156,246}, {251,215,251} },
	/* Rose */
	{ { 30,  8, 17}, { 55, 15, 32}, {246,156,193}, {235, 35,118},
	  {246,156,193}, {251,215,230} },
	/* --- 8 flat palette sets (from the reference swatch image) ------- */
	/* Sunset Berry */
	{ { 28, 19, 17}, { 52, 35, 31}, {219,182,188}, {221,133,115},
	  {240,200,192}, {249,233,230} },
	/* Deep Teal */
	{ {  4, 22, 22}, {  9, 42, 41}, {141,200,195}, {  4,168,166},
	  {142,216,215}, {210,239,239} },
	/* Hot Magenta */
	{ { 30,  7, 17}, { 56, 14, 31}, {195,229,235}, {240, 28,117},
	  {248,153,193}, {252,214,230} },
	/* Golden Hour */
	{ { 30, 25, 14}, { 55, 46, 25}, {185,197,212}, {236,190, 87},
	  {246,226,179}, {252,243,225} },
	/* Meadow */
	{ {  7, 23, 14}, { 13, 43, 27}, {153,198,226}, { 25,175, 93},
	  {152,219,182}, {214,241,226} },
	/* Punch */
	{ { 27, 12, 15}, { 51, 22, 28}, {246,196,155}, {213, 69,102},
	  {236,171,186}, {247,222,227} },
	/* Autumn */
	{ { 29, 14,  9}, { 54, 27, 17}, {248,231,179}, {229, 94, 43},
	  {243,183,160}, {250,226,217} },
	/* Rainforest */
	{ { 31, 16,  5}, { 56, 30, 10}, {156,192,175}, {241,108,  8},
	  {249,189,144}, {252,229,211} }
};

/*
 * White override for the boot intro.
 *
 * The intro needs the wave theme rendered in white. Rather than reimplement
 * the wave field - the first attempt at that came out a pixelated mess,
 * because the real thing is a stack of Gouraud quads with per-layer shading
 * and sparkles, not a few sine lines - this swaps the palette underneath the
 * existing renderer. Same geometry, same sparkles, same everything, just a
 * white set of colours.
 *
 * Not offered in the palette picker: it only makes sense on the intro's white
 * background, where a blue wave field would look wrong.
 */
static const XMBPalette whitePalette = {
	{235, 235, 240}, {248, 248, 252}, {150, 150, 165},
	{200, 200, 215}, {170, 170, 190}, {120, 120, 140}
};

static bool useWhitePalette;

void xmbSetWhitePalette(bool enable) {
	useWhitePalette = enable;
}

static const XMBPalette *currentPalette(void) {
	if (useWhitePalette)
		return &whitePalette;

	uint8_t i = xmbPaletteIndex;
	if (i >= XMB_PALETTE_COUNT)
		i = 0;
	return &xmbPalettes[i];
}

static uint32_t paletteRGB(const uint8_t c[3]) {
	return gp0_rgb(c[0], c[1], c[2]);
}

/* --- icon shading style ------------------------------------------------- */

const char *const xmbIconStyleNames[XMB_ICON_STYLE_COUNT] = {
	"Default", "Light Gradient", "Crystal Glass", "Clear Crystal"
};
uint8_t xmbIconStyle = 0;

/* Scale a palette RGB triple into a GP0 modulation value (0x80 == 1.0x, so
 * modulation is capped there), with a per-channel floor so dark icons never
 * sink below a visible level against the (always very dark) background. */
static uint32_t iconMod(const uint8_t c[3], int num, int den, int floorv) {
	int r = c[0] * num / den, g = c[1] * num / den, b = c[2] * num / den;
	if (r < floorv) r = floorv;
	if (g < floorv) g = floorv;
	if (b < floorv) b = floorv;
	if (r > 0x80) r = 0x80;
	if (g > 0x80) g = 0x80;
	if (b > 0x80) b = 0x80;
	return gp0_rgb(r, g, b);
}

void xmbGetIconGradient(uint32_t *top, uint32_t *bot) {
	const XMBPalette *pal = currentPalette();
	if (xmbIconStyle == 2 || xmbIconStyle == 3) {
		// Crystal Glass: the same material as the memory card tiles and the
		// CD player's track blocks - a bright specular top falling to a deep
		// tint, which reads as a lit glassy surface rather than a flat wash.
		//
		// Follows the theme rather than only the wave palette, so it works on
		// the backgrounds that have no palette of their own: Nebula 3 gives
		// orange crystal, Cosmos violet, PS4 blue.
		uint32_t accent;

		xmbGetAccentColor(&accent, NULL);

		uint32_t r = accent & 0xff;
		uint32_t g = (accent >> 8) & 0xff;
		uint32_t b = (accent >> 16) & 0xff;

		// Highlight: lifted well past the base tint and toward white.
		uint32_t hr = r * 5 / 2 + 0x38;
		uint32_t hg = g * 5 / 2 + 0x38;
		uint32_t hb = b * 5 / 2 + 0x38;

		if (hr > 0xff) hr = 0xff;
		if (hg > 0xff) hg = 0xff;
		if (hb > 0xff) hb = 0xff;

		// Shadow: deeper than the base, floored so the icon never merges
		// into a dark background.
		uint32_t sr = r * 3 / 4 + 0x1c;
		uint32_t sg = g * 3 / 4 + 0x1c;
		uint32_t sb = b * 3 / 4 + 0x1c;

		*top = gp0_rgb((uint8_t) hr, (uint8_t) hg, (uint8_t) hb);
		*bot = gp0_rgb((uint8_t) sr, (uint8_t) sg, (uint8_t) sb);
	} else {
		// Light (also the fallback): bright, light-tinted icons that pop.
		*top = iconMod(pal->crestA, 1, 2, 0x44);
		*bot = iconMod(pal->crestB, 1, 2, 0x38);
	}
}


/* --- UI accent colour --------------------------------------------------- */

/*
 * Dominant hue of each theme, as plain RGB.
 *
 * Screens that draw their own "crystal" tiles - the memory card manager's
 * block grid, the CD player's track list - ask for this so they follow the
 * wallpaper instead of being permanently blue. The values are the colour each
 * background actually reads as on screen, not a guess: Nebula 3's corona and
 * roaming planets are orange, the Cosmos family is violet, the PS5 themes are
 * warm amber, the PS4 ones deep blue.
 *
 * Index 0 ("Default") is the only theme the user can recolour, via the palette
 * picker, so it is handled separately below and this row is never read.
 *
 * Same order as xmbThemeNames[].
 */
static const uint8_t themeAccents[XMB_THEME_COUNT][3] = {
	{  90, 130, 235},   // Default - unused, see xmbGetAccentColor()
	{  90, 130, 235},   // Gouraud Waves + Sparkle - XMB blue
	{  60, 210, 180},   // Aurora - green-teal curtains
	{  80, 150, 240},   // Parallax Ribbons - blue ribbons
	{ 150,  90, 225},   // Space Cosmos - violet nebula
	{ 150,  90, 225},   // Space Cosmos 3D++1
	{ 165, 105, 235},   // Space Cosmos 3D++2
	{ 245, 175,  90},   // PS5 Sparkle - warm amber
	{ 245, 165,  70},   // PS5 Spotlight - amber spotlight
	{ 245, 140,  45},   // Nebula 3 - orange corona
	{  70, 130, 245},   // PS4 - deep blue silk
	{  80, 140, 250}    // PS4 v2 - blue glyphs
};

/*
 * Two colours for a translucent "crystal" tile: a dark base tint and a bright
 * glow for the selected state.
 *
 * The base is deliberately dark. Callers draw it with blending enabled and
 * then lighten it themselves for the sheen and bevel, so handing back the
 * accent at full brightness comes out washed rather than glassy.
 */
void xmbGetAccentColor(uint32_t *base, uint32_t *glow) {
	uint8_t r, g, b;

	if (xmbThemeIndex == XMB_PALETTE_THEME_INDEX) {
		// The one theme with a user-selectable palette: follow the wave
		// crest, which is what gives that background its character.
		const XMBPalette *pal = currentPalette();
		r = pal->crestB[0];
		g = pal->crestB[1];
		b = pal->crestB[2];
	} else {
		uint8_t i = xmbThemeIndex;
		if (i >= XMB_THEME_COUNT)
			i = 0;
		r = themeAccents[i][0];
		g = themeAccents[i][1];
		b = themeAccents[i][2];
	}

	if (base)
		*base = gp0_rgb(r * 3 / 10, g * 3 / 10, b * 3 / 10);

	if (glow) {
		// Bright, and pushed toward white so the bloom reads as light
		// rather than as more of the same colour.
		int gr = r / 2 + 128, gg = g / 2 + 128, gb = b / 2 + 128;
		if (gr > 255) gr = 255;
		if (gg > 255) gg = 255;
		if (gb > 255) gb = 255;
		*glow = gp0_rgb((uint8_t) gr, (uint8_t) gg, (uint8_t) gb);
	}
}

/* --- wave style --------------------------------------------------------- */

uint8_t xmbWaveStyle = 0;

/* Per-style band count and thickness/amplitude scaling for the PSP wave. */
typedef struct {
	int bands;         // how many ribbon bands to draw
	int thickness;     // vertical half-thickness in px
	int ampScale;      // wave amplitude scale, /16 (16 == unchanged)
} XMBWaveStyle;

static const XMBWaveStyle xmbWaveStyles[XMB_WAVE_STYLE_COUNT] = {
	{ 2, 26, 16 },   // Style 1  - 2 fat ribbons
	{ 2, 18, 18 },   // Style 2  - 2 medium
	{ 3, 20, 16 },   // Style 3  - 3 fat
	{ 3, 13, 18 },   // Style 4  - 3 medium
	{ 4, 14, 16 },   // Style 5  - 4 medium
	{ 4,  9, 20 },   // Style 6  - 4 thin
	{ 5, 10, 16 },   // Style 7  - 5 medium
	{ 5,  7, 20 },   // Style 8  - 5 thin
	{ 6,  6, 22 },   // Style 9  - 6 thin
	{ 7,  4, 24 }    // Style 10 - 7 really thin
};

static const XMBWaveStyle *currentWaveStyle(void) {
	uint8_t i = xmbWaveStyle;
	if (i >= XMB_WAVE_STYLE_COUNT)
		i = 0;
	return &xmbWaveStyles[i];
}

/* --- style 11: Gouraud Waves + Sparkle + PSP ----------------------------
 *
 * Derived from the actual PSP XMB background model
 * (system_plugin_bg.rco -> mdl_bg.gmo -> mdl_bg.mds). What that file says:
 *
 *   Model "psp_loop_04_speed1.max"
 *   Bone "J2"  MorphWeights 2 1.000000 0.000000
 *              MultMatrix = rotation of exactly 40 degrees about X
 *   Mesh       DrawBSpline TRIANGLE_STRIP|OPEN 22 11   (22x11 control grid)
 *   Arrays     242 vertices swept along X from -35.97 to 34.80
 *   Texture    diss_00.png, a 128x128 ramp averaging RGB(17,17,45),
 *              running from black up to white
 *
 * So the PSP wave is a ribbon swept along one axis, tilted 40 degrees, and
 * animated by blending between TWO morph targets rather than by scrolling -
 * which is why it reads as a slow breathing fold instead of a moving sine.
 * This theme reproduces that: two wave shapes cross-faded by a morph weight,
 * the 40-degree tilt applied across the sweep, and the model's own
 * navy->white colour ramp, keeping the sparkle layer from Gouraud Sparkle.
 */

#define PSP_TILT_NUM 839   /* tan(40 deg) * 1000, fixed point */

static void drawPSPWave(
	RenderContext *ctx, GPUDMAChain *chain,
	int ampScale,     /* 256 = the original model's vertical extent */
	int heightScale,  /* 256 = the original band thickness          */
	int sparkleCount,
	int bands,        /* number of ribbon layers (original = 2)     */
	int spacing       /* vertical px between stacked bands (0 = original overlap) */
) {
	int w = ctx->screenWidth;
	int h = ctx->screenHeight;
	uint32_t t = xmbFrame;

	const XMBPalette *pal = currentPalette();

	setBlend(chain, GP0_BLEND_SEMITRANS);
	{
		uint32_t top = paletteRGB(pal->bgTop);
		uint32_t bot = paletteRGB(pal->bgBot);
		gouraudQuad(chain,
			0, 0, top,  w, 0, top,
			0, h, bot,  w, h, bot,
			false);
	}

	/* Morph weight 0..256 and back - the MorphWeights pair, not a scroll. */
	int morph = 128 + (isin((t * 2) & (WAVE_FULL - 1)) * 128 >> 12);

	uint32_t black = gp0_rgb(0, 0, 0);
	setBlend(chain, GP0_BLEND_ADD);

	const int SEGS = 22;   /* matches the model's 22-point control row */
	int step = w / SEGS;

	/* shape A: broad shallow fold   shape B: tighter double fold */
	const int A_baseY = 150, A_amp = 26, A_freq = 24, A_h = 70;
	const int B_baseY = 132, B_amp = 15, B_freq = 52, B_h = 58;

	for (int layer = 0; layer < bands; layer++) {
		int phase = layer * 1500;             /* 0, 1500 matches original */
		int speed = (layer & 1) ? 5 : 3;
		uint32_t crest = (layer & 1)
			? paletteRGB(pal->crestB)
			: paletteRGB(pal->crestA);
		/* spread the bands vertically around the centre; spacing 0 keeps the
		 * original overlapping pair exactly. */
		int yOff = spacing * (2 * layer - (bands - 1)) / 2;

		for (int i = 0; i < SEGS; i++) {
			int x0 = i * step;
			int x1 = (i == SEGS - 1) ? w : (i + 1) * step;

			int aA0 = (phase + i       * A_freq + t * speed) & (WAVE_FULL - 1);
			int aA1 = (phase + (i + 1) * A_freq + t * speed) & (WAVE_FULL - 1);
			int aB0 = (phase + i       * B_freq + t * speed) & (WAVE_FULL - 1);
			int aB1 = (phase + (i + 1) * B_freq + t * speed) & (WAVE_FULL - 1);

			int ampA = A_amp * ampScale >> 8;
			int ampB = B_amp * ampScale >> 8;
			int yA0 = A_baseY + (isin(aA0) * ampA >> 12);
			int yB0 = B_baseY + (isin(aB0) * ampB >> 12);
			int yA1 = A_baseY + (isin(aA1) * ampA >> 12);
			int yB1 = B_baseY + (isin(aB1) * ampB >> 12);

			int y0 = (yA0 * (256 - morph) + yB0 * morph) >> 8;
			int y1 = (yA1 * (256 - morph) + yB1 * morph) >> 8;

			/* the model's 40-degree tilt, applied across the sweep */
			y0 -= (x0 * PSP_TILT_NUM) / 4000;
			y1 -= (x1 * PSP_TILT_NUM) / 4000;

			y0 += yOff;
			y1 += yOff;

			int hgt = ((A_h * (256 - morph) + B_h * morph) >> 8)
				* heightScale >> 8;
			if (hgt < 4)
				hgt = 4;

			int b0 = 170 + (isin((aA0 * 2) & (WAVE_FULL - 1)) * 85 >> 12);
			int b1 = 170 + (isin((aA1 * 2) & (WAVE_FULL - 1)) * 85 >> 12);

			gouraudQuad(chain,
				x0, y0,       scaleColor(crest, b0),
				x1, y1,       scaleColor(crest, b1),
				x0, y0 + hgt, black,
				x1, y1 + hgt, black,
				true);
		}
	}

	/* Sparkle layer, as in Gouraud Waves + Sparkle. */
	for (int i = 0; i < sparkleCount; i++) {
		int drift = (i & 1) ? ((int) t * (1 + (i % 3))) / 2 : 0;
		int bx = ((i * 97) - drift) % w;
		if (bx < 0) bx += w;
		int x = bx + (isin((t * 2 + i * 150) & (WAVE_FULL - 1)) * 5 >> 12);
		int y = 84 + ((i * 47) % 104)
			+ (icos((t * 3 + i * 90) & (WAVE_FULL - 1)) * 5 >> 12);

		int tw = 110 + (isin((t * (4 + (i % 5)) + i * 300) & (WAVE_FULL - 1))
			* 145 >> 12);
		if (tw < 0) tw = 0;

		if (i % 3 == 0)
			nebulaBlob(chain, x, y, 5 + (i % 2) * 2,
				scaleColor(paletteRGB(pal->sparkGlow), tw * 3 / 4));
		else
			point(chain, x, y, (i % 4 == 0) ? 2 : 1,
				scaleColor(paletteRGB(pal->sparkDot), tw), true);
	}

	setBlend(chain, GP0_BLEND_SEMITRANS);
}

/* The customizable PSP wave. Wave Style picks band count / thickness /
 * amplitude; Style 1 (index 0) reproduces the original model look. */
static void drawGouraudPSP(RenderContext *ctx, GPUDMAChain *chain) {
	const XMBWaveStyle *ws = currentWaveStyle();
	int heightScale = ws->thickness * 10;      /* px half-thickness -> scale */
	if (heightScale > 256) heightScale = 256;
	int ampScale = ws->ampScale * 16;          /* /16 units -> 256 == x1     */
	int spacing  = ws->thickness * 2 + 4;      /* keep bands from fully merging */
	drawPSPWave(ctx, chain, ampScale, heightScale, 24, ws->bands, spacing);
}

/*
 * "Blue Bend wave" - same base model and 40-degree tilt, but its MDS spans
 * Y -24.4..24.4 against the original's -13.3..9.4, roughly 2.1x the vertical
 * extent. That reads as a much taller, deeper fold, which the larger
 * amplitude scale below reproduces.
 */
static void drawPSPBend(RenderContext *ctx, GPUDMAChain *chain) {
	drawPSPWave(ctx, chain, 545, 300, 20, 2, 0);
}

/*
 * "Blue & Green Thin" - same vertical extent as the original model, but the
 * reference frame is far more subdued (mean RGB 25,30,29 against 18,18,63)
 * and the bands are visibly slimmer, so the thickness is cut right back and
 * more, finer sparkles are used.
 */
static void drawPSPThin(RenderContext *ctx, GPUDMAChain *chain) {
	drawPSPWave(ctx, chain, 230, 105, 30, 2, 0);
}

/* --- style 2: aurora (PS3-ish) ------------------------------------------ */

typedef struct {
	int      cx, amp, freq, speed, halfW;
	uint32_t color;
} AuroraRibbon;

static void drawAurora(RenderContext *ctx, GPUDMAChain *chain) {
	int w = ctx->screenWidth;
	int h = ctx->screenHeight;

	setBlend(chain, GP0_BLEND_SEMITRANS);
	{
		uint32_t top = gp0_rgb(  5,  15,  76); /* deep navy   */
		uint32_t bot = gp0_rgb( 43,  76, 218); /* bright blue */
		gouraudQuad(chain,
			0, 0, top,  w, 0, top,
			0, h, bot,  w, h, bot,
			false);
	}

	AuroraRibbon rib[5];
	rib[0] = (AuroraRibbon){  55, 65, 24,  7, 26, gp0_rgb(150, 210, 255) }; /* pale ice blue */
	rib[1] = (AuroraRibbon){ 120, 70, 26,  8, 30, gp0_rgb(120, 190, 255) }; /* sky blue      */
	rib[2] = (AuroraRibbon){ 190, 84, 20, 10, 34, gp0_rgb( 90, 150, 245) }; /* azure         */
	rib[3] = (AuroraRibbon){ 255, 60, 34,  6, 26, gp0_rgb( 60, 110, 230) }; /* royal blue    */
	rib[4] = (AuroraRibbon){ 300, 50, 30,  9, 22, gp0_rgb( 70,  85, 200) }; /* indigo        */

	const int SEG = 12;
	int step = h / SEG;
	uint32_t black = gp0_rgb(0, 0, 0);

	setBlend(chain, GP0_BLEND_ADD);

	for (int r = 0; r < 5; r++) {
		AuroraRibbon *R = &rib[r];
		for (int i = 0; i < SEG; i++) {
			int y0 = i * step;
			int y1 = (i == SEG - 1) ? h : (i + 1) * step;

			int a0 = (i       * R->freq + xmbFrame * R->speed) & (WAVE_FULL - 1);
			int a1 = ((i + 1) * R->freq + xmbFrame * R->speed) & (WAVE_FULL - 1);
			int x0 = R->cx + (isin(a0) * R->amp >> 12);
			int x1 = R->cx + (isin(a1) * R->amp >> 12);

			/* Bright core in the middle, black at both edges: draw as two
			 * quads (left edge->core, core->right edge). */
			gouraudQuad(chain,
				x0 - R->halfW, y0, black, x0, y0, R->color,
				x1 - R->halfW, y1, black, x1, y1, R->color,
				true);
			gouraudQuad(chain,
				x0, y0, R->color, x0 + R->halfW, y0, black,
				x1, y1, R->color, x1 + R->halfW, y1, black,
				true);
		}
	}

	setBlend(chain, GP0_BLEND_SEMITRANS);
}

/* --- style 3/4: Space Cosmos (PS2-intro inspired) ----------------------- */

/* A soft radial glow built from a triangle fan: bright centre fading to
 * black at the rim. Additive, so overlapping glows build up a nebula. */
static void nebulaBlob(
	GPUDMAChain *chain, int cx, int cy, int radius, uint32_t centre
) {
	const int RINGS = 8;
	uint32_t black = gp0_rgb(0, 0, 0);
	int px = cx + radius, py = cy; /* angle 0 */

	for (int k = 1; k <= RINGS; k++) {
		int a  = (k * (WAVE_FULL / RINGS)) & (WAVE_FULL - 1);
		int nx = cx + (icos(a) * radius >> 12);
		int ny = cy + (isin(a) * radius >> 12);
		gouraudTri(chain, cx, cy, centre, px, py, black, nx, ny, black, true);
		px = nx; py = ny;
	}
}

/* Same fan-based glow as nebulaBlob(), but the rim radius is perturbed per
 * vertex by a fast sine wobble - Gemini's "writhing solar flare" trick:
 *
 *     radius_offset = sin(angle * 8 + frame * speed) * flareAmount
 *
 * so the outer edge pulses and licks outward unevenly instead of staying a
 * perfect circle, like a real corona/heat-haze, while the centre-to-black
 * additive gradient (and therefore the "glow" read) is identical to
 * nebulaBlob() - same primitive count, same cost, just per-vertex CPU trig
 * that's free relative to the GPU chain budget. flareAmount == 0 makes this
 * pixel-identical to nebulaBlob(), so it's safe to use unconditionally in
 * place of it. */
static void nebulaBlobWobbly(
	GPUDMAChain *chain, int cx, int cy, int radius, uint32_t centre,
	uint32_t t, int flareAmount
) {
	const int RINGS = 8;
	uint32_t black = gp0_rgb(0, 0, 0);
	uint32_t creep = t * 26;   // precession speed of the flare pattern

	int a0   = 0;
	int wob0 = flareAmount
		? (isin((a0 * 8 + (int) creep) & (WAVE_FULL - 1)) * flareAmount >> 12)
		: 0;
	int px = cx + radius + wob0, py = cy;

	for (int k = 1; k <= RINGS; k++) {
		int a   = (k * (WAVE_FULL / RINGS)) & (WAVE_FULL - 1);
		int wob = flareAmount
			? (isin((a * 8 + (int) creep) & (WAVE_FULL - 1)) * flareAmount >> 12)
			: 0;
		int r  = radius + wob;
		int nx = cx + (icos(a) * r >> 12);
		int ny = cy + (isin(a) * r >> 12);
		gouraudTri(chain, cx, cy, centre, px, py, black, nx, ny, black, true);
		px = nx; py = ny;
	}
}

/* One ghostly isometric cube (top + left + right faces), semi-transparent.
 * Cheap 2D filler used alongside the real GTE cubes. */
static void isoCube(GPUDMAChain *chain, int x, int y, int s) {
	int hs = s / 2;
	uint32_t top   = gp0_rgb(150, 165, 190);
	uint32_t left  = gp0_rgb( 70,  85, 120);
	uint32_t right = gp0_rgb( 40,  52,  78);

	flatQuad(chain, x, y - s,  x + s, y - hs,  x - s, y - hs,  x, y,      top,   true);
	flatQuad(chain, x - s, y - hs,  x, y,  x - s, y + hs,  x, y + s,      left,  true);
	flatQuad(chain, x, y,  x + s, y - hs,  x, y + s,  x + s, y + hs,      right, true);
}

/* Shared breathing blue nebula. */
static void drawNebula(GPUDMAChain *chain, int cx, int cy, uint32_t t) {
	setBlend(chain, GP0_BLEND_ADD);
	int pulse = 100 + (isin((t * 4) & (WAVE_FULL - 1)) * 40 >> 12); /* 60..140% */
	uint32_t core  = scaleColor(gp0_rgb(30, 55, 130), pulse);
	uint32_t core2 = scaleColor(gp0_rgb(24, 40, 110), pulse);
	int dx = isin((t * 3) & (WAVE_FULL - 1)) * 20 >> 12;
	int dy = icos((t * 2) & (WAVE_FULL - 1)) * 14 >> 12;
	nebulaBlob(chain, cx + dx,      cy + dy,      96, core);
	nebulaBlob(chain, cx - 34 - dy, cy + 10 + dx, 70, core2);
	nebulaBlob(chain, cx + 40 + dx, cy - 20 - dy, 60, core2);
}

/* Nebula that slowly morphs between the soft "smoke" shape drawNebula()
 * above draws (three overlapping drifting blobs) and a tighter, radiating
 * "star" shape (a bright core plus a handful of thin rays swept around it) -
 * a slow crossfade between the two driven by a long, independent timer, used
 * by the two Cosmos 3D++ themes in place of the plain drawNebula().
 *
 * RECONSTRUCTION NOTE: this function was accidentally deleted along with the
 * old "Space Cosmos 3D" / "Space Cosmos 3D+" themes it happened to sit
 * between (both since removed) and has been rebuilt here from its call sites
 * and drawNebula()'s pattern just above - the morph behaviour is faithful to
 * its own doc comment, but if this doesn't match what Cosmos 3D++1/++2 used
 * to look like, that mismatch is exactly why: the original body wasn't
 * preserved anywhere before it was overwritten. */
static void drawNebulaMorph(GPUDMAChain *chain, int cx, int cy, uint32_t t) {
	setBlend(chain, GP0_BLEND_ADD);

	// Slow morph factor, independent of the drift/pulse timers below so the
	// two shapes cross-fade at their own unhurried pace: 0 = pure smoke,
	// 4096 = pure star.
	int morph = (isin((t * 1 + 1024) & (WAVE_FULL - 1)) + 4096) >> 1; // 0..4096

	int pulse = 100 + (isin((t * 4) & (WAVE_FULL - 1)) * 40 >> 12); /* 60..140% */
	int dx = isin((t * 3) & (WAVE_FULL - 1)) * 20 >> 12;
	int dy = icos((t * 2) & (WAVE_FULL - 1)) * 14 >> 12;

	// Smoke component: same three drifting blobs as drawNebula(), faded out
	// as morph increases.
	int smokeAmt = 4096 - morph;
	if (smokeAmt > 0) {
		uint32_t sc1 = scaleColor(gp0_rgb(30, 55, 130), pulse * smokeAmt >> 12);
		uint32_t sc2 = scaleColor(gp0_rgb(24, 40, 110), pulse * smokeAmt >> 12);
		nebulaBlob(chain, cx + dx,      cy + dy,      96, sc1);
		nebulaBlob(chain, cx - 34 - dy, cy + 10 + dx, 70, sc2);
		nebulaBlob(chain, cx + 40 + dx, cy - 20 - dy, 60, sc2);
	}

	// Star component: a bright compact core plus 4 thin rays swept slowly
	// around it, faded in as morph increases.
	if (morph > 0) {
		uint32_t bc = scaleColor(gp0_rgb(60, 110, 200), pulse * morph >> 12);
		uint32_t bd = scaleColor(gp0_rgb(30, 60, 130),  pulse * morph >> 12);
		nebulaBlob(chain, cx, cy, 42, bc);
		nebulaBlob(chain, cx, cy, 70, bd);

		uint32_t bs = scaleColor(gp0_rgb(90, 150, 230), pulse * morph >> 12);
		for (int i = 0; i < 4; i++) {
			int a = ((int) t * 5 + i * (WAVE_FULL / 4)) & (WAVE_FULL - 1);
			int sdx = icos(a), sdy = isin(a);
			int x1 = cx + (sdx * 46 >> 12), y1 = cy + (sdy * 46 >> 12);
			int x2 = cx + (sdx * 84 >> 12), y2 = cy + (sdy * 84 >> 12);
			nebulaBlob(chain, x1, y1, 30, bs);
			nebulaBlob(chain, x2, y2, 20, bd);
		}
	}
}

/* Comet-star definitions, shared by the star renderer and the refraction
 * theme. Colours are raw RGB (gp0_rgb() isn't usable in a static initialiser). */
static const struct {
	int ax, ay, sx, sy, phx, phy;
	uint8_t r, g, b;
} STARS[5] = {
	{ 130,  80, 3, 5,    0,  400, 255,  50,  50 },
	{ 150, 100, 5, 3,  900, 1500,  50, 220,  70 },
	{ 110, 110, 4, 6, 1800,  600, 220,  60, 220 },
	{ 160,  70, 6, 4, 2600, 2000,  60, 210, 235 },
	{ 120,  95, 5, 5, 3300, 1000,  90, 130, 255 },
};

static inline int starHeadX(int i, int cx, uint32_t t) {
	return cx + (icos((t * STARS[i].sx + STARS[i].phx) & (WAVE_FULL - 1))
		* STARS[i].ax >> 12);
}
static inline int starHeadY(int i, int cy, uint32_t t) {
	return cy + (isin((t * STARS[i].sy + STARS[i].phy) & (WAVE_FULL - 1))
		* STARS[i].ay >> 12);
}

/* Shared colored comet-stars with LONG PS2-style trails. Each trail is drawn
 * as a chain of additive Gouraud line segments sampled along the star's past
 * path, fading from a bright head to a dark tail. */
static void drawStars(GPUDMAChain *chain, int cx, int cy, uint32_t t) {
	uint32_t cols[5];
	for (int i = 0; i < 5; i++)
		cols[i] = gp0_rgb(STARS[i].r, STARS[i].g, STARS[i].b);

	const int SEGS = 8;
	const int SPAN = 56; /* frames of history -> long trail */

	setBlend(chain, GP0_BLEND_ADD);

	for (int i = 0; i < 5; i++) {
		int hx = 0, hy = 0;

		for (int seg = 0; seg < SEGS; seg++) {
			int t0 = (int) t - (SPAN * seg)       / SEGS;
			int t1 = (int) t - (SPAN * (seg + 1)) / SEGS;

			int x0 = cx + (icos((t0 * STARS[i].sx + STARS[i].phx) & (WAVE_FULL - 1)) * STARS[i].ax >> 12);
			int y0 = cy + (isin((t0 * STARS[i].sy + STARS[i].phy) & (WAVE_FULL - 1)) * STARS[i].ay >> 12);
			int x1 = cx + (icos((t1 * STARS[i].sx + STARS[i].phx) & (WAVE_FULL - 1)) * STARS[i].ax >> 12);
			int y1 = cy + (isin((t1 * STARS[i].sy + STARS[i].phy) & (WAVE_FULL - 1)) * STARS[i].ay >> 12);

			int b0 = 256 - (256 * seg)       / SEGS; /* head end of segment */
			int b1 = 256 - (256 * (seg + 1)) / SEGS; /* tail end            */

			shadedLine(chain,
				x0, y0, scaleColor(cols[i], b0),
				x1, y1, scaleColor(cols[i], b1),
				true);

			if (seg == 0) { hx = x0; hy = y0; }
		}

		point(chain, hx, hy, 2, cols[i], true);
	}
}

/* Shared near-black space wash. */
static void cosmosWash(GPUDMAChain *chain, int w, int h) {
	setBlend(chain, GP0_BLEND_SEMITRANS);
	uint32_t top = gp0_rgb(2, 2, 6);
	uint32_t bot = gp0_rgb(4, 5, 16);
	gouraudQuad(chain,
		0, 0, top,  w, 0, top,
		0, h, bot,  w, h, bot,
		false);
}

/* Cheap fake cubes orbiting the centre (used by the non-3D cosmos). */
static void drawFakeCubes(GPUDMAChain *chain, int cx, int cy, uint32_t t) {
	static const struct { int orbit, sizeBase, speed, phase; } cubes[] = {
		{ 120, 12, 2,    0 },
		{  86, 16, 3,  900 },
		{ 140,  9, 2, 1700 },
		{  60, 14, 4, 2500 },
		{ 150, 10, 3, 3300 },
		{  40, 11, 5, 1200 },
	};
	setBlend(chain, GP0_BLEND_SEMITRANS);
	for (int i = 0; i < 6; i++) {
		int a = (t * cubes[i].speed + cubes[i].phase) & (WAVE_FULL - 1);
		int x = cx + (icos(a) * cubes[i].orbit >> 12);
		int y = cy + (isin(a) * (cubes[i].orbit * 3 / 5) >> 12);
		int s = cubes[i].sizeBase + (isin((a * 2) & (WAVE_FULL - 1)) * 3 >> 12);
		isoCube(chain, x, y, s);
	}
}

static void drawCosmos(RenderContext *ctx, GPUDMAChain *chain) {
	int w = ctx->screenWidth, h = ctx->screenHeight;
	int cx = w / 2, cy = h / 2;
	uint32_t t = xmbFrame;

	cosmosWash(chain, w, h);
	drawNebula(chain, cx, cy, t);
	drawFakeCubes(chain, cx, cy, t);
	drawStars(chain, cx, cy, t);          /* long trails, drawn on top */
	setBlend(chain, GP0_BLEND_SEMITRANS);
}

/* --- real GTE cubes (for the 3D cosmos variant) ------------------------- */

#define GTE_UNIT (1 << 12)

static void cosmosMulMatrixByVectors(GTEMatrix *output) {
	gte_command(GTE_CMD_MVMVA | GTE_SF | GTE_MX_RT | GTE_V_V0 | GTE_CV_NONE);
	output->values[0][0] = (int16_t) gte_getDataReg(GTE_IR1);
	output->values[1][0] = (int16_t) gte_getDataReg(GTE_IR2);
	output->values[2][0] = (int16_t) gte_getDataReg(GTE_IR3);
	gte_command(GTE_CMD_MVMVA | GTE_SF | GTE_MX_RT | GTE_V_V1 | GTE_CV_NONE);
	output->values[0][1] = (int16_t) gte_getDataReg(GTE_IR1);
	output->values[1][1] = (int16_t) gte_getDataReg(GTE_IR2);
	output->values[2][1] = (int16_t) gte_getDataReg(GTE_IR3);
	gte_command(GTE_CMD_MVMVA | GTE_SF | GTE_MX_RT | GTE_V_V2 | GTE_CV_NONE);
	output->values[0][2] = (int16_t) gte_getDataReg(GTE_IR1);
	output->values[1][2] = (int16_t) gte_getDataReg(GTE_IR2);
	output->values[2][2] = (int16_t) gte_getDataReg(GTE_IR3);
}

static void cosmosRotate(int yaw, int pitch, int roll) {
	static GTEMatrix multiplied;
	int s, c;
	if (yaw) {
		s = isin(yaw); c = icos(yaw);
		gte_setColumnVectors(c, -s, 0,  s, c, 0,  0, 0, GTE_UNIT);
		cosmosMulMatrixByVectors(&multiplied);
		gte_loadRotationMatrix(&multiplied);
	}
	if (pitch) {
		s = isin(pitch); c = icos(pitch);
		gte_setColumnVectors(c, 0, s,  0, GTE_UNIT, 0,  -s, 0, c);
		cosmosMulMatrixByVectors(&multiplied);
		gte_loadRotationMatrix(&multiplied);
	}
	if (roll) {
		s = isin(roll); c = icos(roll);
		gte_setColumnVectors(GTE_UNIT, 0, 0,  0, c, -s,  0, s, c);
		cosmosMulMatrixByVectors(&multiplied);
		gte_loadRotationMatrix(&multiplied);
	}
}

static void setupCosmosGTE(int w, int h) {
	cop0_setReg(COP0_STATUS, cop0_getReg(COP0_STATUS) | COP0_STATUS_CU2);
	gte_setControlReg(GTE_OFX, (w << 16) / 2);
	gte_setControlReg(GTE_OFY, (h << 16) / 2);
	int focalLength = (w < h) ? w : h;
	gte_setControlReg(GTE_H, focalLength / 2);
}

static const GTEVector16 cbVerts[8] = {
	{ .x = -1, .y = -1, .z = -1 }, { .x =  1, .y = -1, .z = -1 },
	{ .x = -1, .y =  1, .z = -1 }, { .x =  1, .y =  1, .z = -1 },
	{ .x = -1, .y = -1, .z =  1 }, { .x =  1, .y = -1, .z =  1 },
	{ .x = -1, .y =  1, .z =  1 }, { .x =  1, .y =  1, .z =  1 }
};
static const uint8_t cbFaces[6][4] = {
	{ 0, 1, 2, 3 }, { 6, 7, 4, 5 }, { 4, 5, 0, 1 },
	{ 7, 6, 3, 2 }, { 6, 4, 2, 0 }, { 5, 7, 1, 3 }
};
/* per-face grey levels -> ghostly bluish faces */
static const uint8_t cbShade[6] = { 160, 100, 175, 90, 135, 115 };

/* Draw one real cube of half-extent `half` (world units): GTE transform +
 * backface cull, emitted as semi-transparent flat quads. Convex + backface
 * cull means the visible faces never overlap, so no ordering table is needed. */
static void drawRealCube(
	GPUDMAChain *chain, int wx, int wy, int wz,
	int yaw, int pitch, int roll, int half
) {
	GTEVector16 v[8];
	for (int k = 0; k < 8; k++) {
		v[k].x = (int16_t)(cbVerts[k].x * half);
		v[k].y = (int16_t)(cbVerts[k].y * half);
		v[k].z = (int16_t)(cbVerts[k].z * half);
	}

	gte_setControlReg(GTE_TRX, wx);
	gte_setControlReg(GTE_TRY, wy);
	gte_setControlReg(GTE_TRZ, wz);
	gte_setRotationMatrix(
		GTE_UNIT, 0, 0,  0, GTE_UNIT, 0,  0, 0, GTE_UNIT
	);
	cosmosRotate(yaw, pitch, roll);

	for (int f = 0; f < 6; f++) {
		gte_loadV0(&v[cbFaces[f][0]]);
		gte_loadV1(&v[cbFaces[f][1]]);
		gte_loadV2(&v[cbFaces[f][2]]);
		gte_command(GTE_CMD_RTPT | GTE_SF);

		gte_command(GTE_CMD_NCLIP);
		if (((int) gte_getDataReg(GTE_MAC0)) <= 0)
			continue; /* backface */

		uint32_t xy0 = gte_getDataReg(GTE_SXY0);

		gte_loadV0(&v[cbFaces[f][3]]);
		gte_command(GTE_CMD_RTPS | GTE_SF);

		int sh = cbShade[f];
		uint32_t col = gp0_rgb((sh * 13) >> 4, (sh * 15) >> 4, sh);

		uint32_t *ptr = allocateGP0Packet(chain, 5);
		ptr[0] = col | gp0_shadedQuad(false, false, true);
		ptr[1] = xy0;
		gte_storeDataReg(GTE_SXY0, 2 * 4, ptr);
		gte_storeDataReg(GTE_SXY1, 3 * 4, ptr);
		gte_storeDataReg(GTE_SXY2, 4 * 4, ptr);
	}
}

/* Draw one cube as fake GLASS, using the classic PS1 tricks (Spyro's fake
 * environment map, FF9's darker core + additive shell, Crash's extreme vertex
 * colouring) adapted to our untextured GTE cubes:
 *
 *   Pass 1 - CORE:  the visible faces as a dark, saturated blue-cyan
 *                   semi-transparent quad. This is the "solid darker inner
 *                   shape" you see through the glass.
 *   Pass 2 - SHEEN: the SAME faces again, additively blended, with each of the
 *                   four corners given its own brightness from a highlight
 *                   that sweeps around the face over time (independent of the
 *                   cube's own tumble - that's the fake environment map). The
 *                   additive icy-white glint sliding across the faces is what
 *                   reads as reflective/refractive glass.
 *
 * The GTE transform and backface cull are shared between both passes (we only
 * transform each face once), so this stays cheap. */
static void drawGlassCube(
	GPUDMAChain *chain, int wx, int wy, int wz,
	int yaw, int pitch, int roll, int half, uint32_t sweep
) {
	GTEVector16 v[8];
	for (int k = 0; k < 8; k++) {
		v[k].x = (int16_t)(cbVerts[k].x * half);
		v[k].y = (int16_t)(cbVerts[k].y * half);
		v[k].z = (int16_t)(cbVerts[k].z * half);
	}

	gte_setControlReg(GTE_TRX, wx);
	gte_setControlReg(GTE_TRY, wy);
	gte_setControlReg(GTE_TRZ, wz);
	gte_setRotationMatrix(
		GTE_UNIT, 0, 0,  0, GTE_UNIT, 0,  0, 0, GTE_UNIT
	);
	cosmosRotate(yaw, pitch, roll);

	for (int f = 0; f < 6; f++) {
		gte_loadV0(&v[cbFaces[f][0]]);
		gte_loadV1(&v[cbFaces[f][1]]);
		gte_loadV2(&v[cbFaces[f][2]]);
		gte_command(GTE_CMD_RTPT | GTE_SF);

		gte_command(GTE_CMD_NCLIP);
		if (((int) gte_getDataReg(GTE_MAC0)) <= 0)
			continue; /* backface */

		uint32_t xy0 = gte_getDataReg(GTE_SXY0);

		gte_loadV0(&v[cbFaces[f][3]]);
		gte_command(GTE_CMD_RTPS | GTE_SF);
		/* SXY0/1/2 now hold face vertices 1,2,3; xy0 holds vertex 0. These
		 * stay valid until the next GTE command, so both passes reuse them. */

		/* --- pass 1: dark saturated core (semi-transparent) --- */
		setBlend(chain, GP0_BLEND_SEMITRANS);
		{
			int sh = cbShade[f];
			/* dim, deep blue-cyan: the inner shape seen through the glass */
			uint32_t core = gp0_rgb(
				(sh * 3) >> 4,     /* very little red   */
				(sh * 7) >> 4,     /* some green (cyan) */
				(sh * 12) >> 4);   /* strong blue       */

			uint32_t *ptr = allocateGP0Packet(chain, 5);
			ptr[0] = core | gp0_shadedQuad(false, false, true);
			ptr[1] = xy0;
			gte_storeDataReg(GTE_SXY0, 2 * 4, ptr);
			gte_storeDataReg(GTE_SXY1, 3 * 4, ptr);
			gte_storeDataReg(GTE_SXY2, 4 * 4, ptr);
		}

		/* --- pass 2: additive sheen (Gouraud, sweeping highlight) --- */
		setBlend(chain, GP0_BLEND_ADD);
		{
			uint32_t cc[4];
			for (int k = 0; k < 4; k++) {
				/* corner phase spreads the highlight around the quad; the
				 * per-face offset keeps neighbouring faces out of phase so
				 * the glint travels across the whole cube. */
				int ang = (sweep + f * 680 + k * (WAVE_FULL / 4))
					& (WAVE_FULL - 1);
				/* brightness 30..165: (isin+4096) is 0..8192 */
				int b = 30 + ((isin(ang) + (1 << 12)) * 135 >> 13);
				/* icy blue-white glint */
				cc[k] = gp0_rgb(
					(b * 11) >> 4,   /* ~0.69 r */
					(b * 14) >> 4,   /* ~0.88 g */
					b);              /* full    b */
			}

			uint32_t *ptr = allocateGP0Packet(chain, 8);
			ptr[0] = cc[0] | gp0_shadedQuad(true, false, true);
			ptr[1] = xy0;
			ptr[2] = cc[1];
			gte_storeDataReg(GTE_SXY0, 3 * 4, ptr);
			ptr[4] = cc[2];
			gte_storeDataReg(GTE_SXY1, 5 * 4, ptr);
			ptr[6] = cc[3];
			gte_storeDataReg(GTE_SXY2, 7 * 4, ptr);
		}
	}
}

/* --- style 9: Space Cosmos 3D++1 (refraction glow) ---------------------- */

/* Same as 3D+, but when a comet-star passes behind a cube the cube picks up a
 * soft additive glow in that star's colour - a cheap fake of the star's light
 * refracting through the translucent cube. */
static void drawCosmos3DPlusPlus(RenderContext *ctx, GPUDMAChain *chain) {
	int w = ctx->screenWidth, h = ctx->screenHeight;
	int cx = w / 2, cy = h / 2;
	uint32_t t = xmbFrame;
	int focal = ((w < h) ? w : h) / 2;

	cosmosWash(chain, w, h);
	drawNebulaMorph(chain, cx, cy, t);
	drawStars(chain, cx, cy, t);

	setBlend(chain, GP0_BLEND_SEMITRANS);
	{
		static const struct { int bx, by, size, sp, ph; } FK[] = {
			{  52,  58,  8, 2,    0 }, { 266,  78,  7, 3,  800 },
			{  42, 182,  9, 2, 1600 }, { 280, 168,  6, 4, 2400 },
			{ 158,  42,  7, 3, 3200 },
		};
		for (int i = 0; i < 5; i++) {
			int a = (t * FK[i].sp + FK[i].ph) & (WAVE_FULL - 1);
			isoCube(chain, FK[i].bx + (icos(a) * 10 >> 12),
				FK[i].by + (isin(a) * 8 >> 12), FK[i].size);
		}
	}

	// Re-arm the GTE every frame. It is only four control-register writes,
	// and other screens (notably the spinning cube test) reconfigure the
	// GTE for their own projection/ordering table - so caching this in a
	// one-shot flag meant the cosmos themes could silently inherit foreign
	// GTE state after returning from those screens.
	setupCosmosGTE(w, h);

	struct { int orbit, zbase, ospeed, ophase; } RC[3] = {
		{ 130, 250, 1,    0 },
		{ 165, 290, 1, 1300 },
		{ 100, 220, 2, 2600 },
	};
	int wx[3], wy[3], wz[3], yaw[3], pit[3], rol[3], order[3];
	for (int i = 0; i < 3; i++) {
		int a = (t * RC[i].ospeed + RC[i].ophase) & (WAVE_FULL - 1);
		wx[i] = icos(a) * RC[i].orbit >> 12;
		wy[i] = isin(a) * (RC[i].orbit * 3 / 5) >> 12;
		wz[i] = RC[i].zbase + (isin((a * 2) & (WAVE_FULL - 1)) * 40 >> 12);
		yaw[i] = (int) t * 2 + i * 400;
		pit[i] = (int) t * 1 + i * 700;
		rol[i] = 0;
		order[i] = i;
	}
	for (int a = 0; a < 2; a++)
		for (int b = a + 1; b < 3; b++)
			if (wz[order[b]] > wz[order[a]]) {
				int tmp = order[a]; order[a] = order[b]; order[b] = tmp;
			}

	/* Glass cubes: dark core + sweeping additive sheen (see drawGlassCube).
	 * The sheen sweep is time-driven so the glint slides across the glass as
	 * the cubes tumble, with a per-cube phase offset so they don't glint in
	 * unison. */
	for (int k = 0; k < 3; k++) {
		int i = order[k];
		drawGlassCube(chain, wx[i], wy[i], wz[i], yaw[i], pit[i], rol[i], 19,
			t * 6 + i * 1300);
	}

	/* Refraction glow: for each cube, if a star head is over it, add a soft
	 * glow in the star's colour, brighter the closer the star is to centre. */
	setBlend(chain, GP0_BLEND_ADD);
	for (int k = 0; k < 3; k++) {
		int i   = order[k];
		int scx = cx + wx[i] * focal / wz[i];
		int scy = cy + wy[i] * focal / wz[i];
		int crad = 19 * focal / wz[i];        /* cube half-size on screen  */
		int reach = crad + 14;

		for (int s = 0; s < 5; s++) {
			int hx  = starHeadX(s, cx, t);
			int hy  = starHeadY(s, cy, t);
			int ddx = hx - scx, ddy = hy - scy;
			int d2  = ddx * ddx + ddy * ddy;
			if (d2 >= reach * reach)
				continue;

			int prox = 256 - (d2 * 256 / (reach * reach)); /* 0..256 */
			uint32_t col = scaleColor(
				gp0_rgb(STARS[s].r, STARS[s].g, STARS[s].b), prox);
			nebulaBlob(chain, scx, scy, crad + 6, col);
		}
	}

	setBlend(chain, GP0_BLEND_SEMITRANS);
}

/* --- style 10: Space Cosmos 3D++2 (more drifting 2D cubes) -------------- */

static void drawCosmos3DPlusPlus2(RenderContext *ctx, GPUDMAChain *chain) {
	int w = ctx->screenWidth, h = ctx->screenHeight;
	int cx = w / 2, cy = h / 2;
	uint32_t t = xmbFrame;
	int focal = ((w < h) ? w : h) / 2;

	cosmosWash(chain, w, h);
	drawNebulaMorph(chain, cx, cy, t);
	drawStars(chain, cx, cy, t);

	setBlend(chain, GP0_BLEND_SEMITRANS);
	{
		/*
		 * More 2D cubes than 3D++1, animated like the original Space
		 * Cosmos: each swings toward and away from the viewer so it grows
		 * and shrinks noticeably, and drifts faster. They sit around the
		 * edges - the middle is left to the real GTE cubes so the two
		 * sets do not fight for attention.
		 */
		static const struct { int bx, by, size, sp, ph; } FK[] = {
			{  38,  52,  9, 4,    0 }, { 286,  64,  8, 5,  600 },
			{  30, 176, 10, 4, 1200 }, { 292, 186,  7, 6, 1800 },
			{ 150,  34,  8, 5, 2400 }, {  62, 212,  7, 5, 3000 },
			{ 262, 124,  9, 4, 3600 }, {  24, 116,  8, 6,  900 },
			{ 178, 216,  7, 5, 2100 },
		};
		for (int i = 0; i < 9; i++) {
			int a  = (t * FK[i].sp + FK[i].ph) & (WAVE_FULL - 1);
			int sz = FK[i].size + (isin(a) * 4 >> 12);   /* near/far swing */
			if (sz < 3)
				sz = 3;
			isoCube(chain, FK[i].bx + (icos(a) * 14 >> 12),
				FK[i].by + (isin((a * 2) & (WAVE_FULL - 1)) * 11 >> 12), sz);
		}
	}

	// Re-arm the GTE every frame. It is only four control-register writes,
	// and other screens (notably the spinning cube test) reconfigure the
	// GTE for their own projection/ordering table - so caching this in a
	// one-shot flag meant the cosmos themes could silently inherit foreign
	// GTE state after returning from those screens.
	setupCosmosGTE(w, h);

	struct { int orbit, zbase, ospeed, ophase; } RC[3] = {
		{ 130, 250, 1,    0 },
		{ 165, 290, 1, 1300 },
		{ 100, 220, 2, 2600 },
	};
	int wx[3], wy[3], wz[3], yaw[3], pit[3], rol[3], order[3];
	for (int i = 0; i < 3; i++) {
		int a = (t * RC[i].ospeed + RC[i].ophase) & (WAVE_FULL - 1);
		wx[i] = icos(a) * RC[i].orbit >> 12;
		wy[i] = isin(a) * (RC[i].orbit * 3 / 5) >> 12;
		wz[i] = RC[i].zbase + (isin((a * 2) & (WAVE_FULL - 1)) * 40 >> 12);
		yaw[i] = (int) t * 2 + i * 400;
		pit[i] = (int) t * 1 + i * 700;
		rol[i] = 0;
		order[i] = i;
	}
	for (int a = 0; a < 2; a++)
		for (int b = a + 1; b < 3; b++)
			if (wz[order[b]] > wz[order[a]]) {
				int tmp = order[a]; order[a] = order[b]; order[b] = tmp;
			}

	setBlend(chain, GP0_BLEND_SEMITRANS);
	for (int k = 0; k < 3; k++) {
		int i = order[k];
		drawRealCube(chain, wx[i], wy[i], wz[i], yaw[i], pit[i], rol[i], 19);
	}

	/* Refraction glow: for each cube, if a star head is over it, add a soft
	 * glow in the star's colour, brighter the closer the star is to centre. */
	setBlend(chain, GP0_BLEND_ADD);
	for (int k = 0; k < 3; k++) {
		int i   = order[k];
		int scx = cx + wx[i] * focal / wz[i];
		int scy = cy + wy[i] * focal / wz[i];
		int crad = 19 * focal / wz[i];        /* cube half-size on screen  */
		int reach = crad + 14;

		for (int s = 0; s < 5; s++) {
			int hx  = starHeadX(s, cx, t);
			int hy  = starHeadY(s, cy, t);
			int ddx = hx - scx, ddy = hy - scy;
			int d2  = ddx * ddx + ddy * ddy;
			if (d2 >= reach * reach)
				continue;

			int prox = 256 - (d2 * 256 / (reach * reach)); /* 0..256 */
			uint32_t col = scaleColor(
				gp0_rgb(STARS[s].r, STARS[s].g, STARS[s].b), prox);
			nebulaBlob(chain, scx, scy, crad + 6, col);
		}
	}

	setBlend(chain, GP0_BLEND_SEMITRANS);
}

static void drawPS5Sparkle(RenderContext *ctx, GPUDMAChain *chain) {
	int w = ctx->screenWidth, h = ctx->screenHeight;
	uint32_t t = xmbFrame;

	/* Dark, faintly warm wash. */
	setBlend(chain, GP0_BLEND_SEMITRANS);
	{
		uint32_t top = gp0_rgb(10,  8,  7);
		uint32_t bot = gp0_rgb( 3,  3,  5);
		gouraudQuad(chain,
			0, 0, top,  w, 0, top,
			0, h, bot,  w, h, bot,
			false);
	}

	/* Soft light ray sweeping from the top-left, gently swaying + pulsing. */
	setBlend(chain, GP0_BLEND_ADD);
	{
		uint32_t black = gp0_rgb(0, 0, 0);
		int sway  = isin((t * 3) & (WAVE_FULL - 1)) * 10 >> 12;
		int pulse = 100 + (isin((t * 5) & (WAVE_FULL - 1)) * 40 >> 12);
		uint32_t ray  = scaleColor(gp0_rgb(120, 105, 72), pulse);
		uint32_t rayI = scaleColor(gp0_rgb(150, 135, 96), pulse);

		/* wide outer beam */
		gouraudQuad(chain,
			30 + sway, -12, ray,   95 + sway, -12, ray,
			-50,        h,  black, 150,        h,  black,
			true);
		/* bright inner beam */
		gouraudQuad(chain,
			48 + sway, -12, rayI,  78 + sway, -12, rayI,
			20,         h,  black, 120,        h,  black,
			true);
	}

	/* Warm sparkling particles drifting slowly upward, twinkling. */
	{
		const int N = 30;
		int period = h + 40;
		for (int i = 0; i < N; i++) {
			int bx = (i * 79) % w
				+ (isin((t * 2 + i * 130) & (WAVE_FULL - 1)) * 4 >> 12);
			int drift = ((int) t * (1 + (i % 3)) + i * 47) % period;
			int y  = h - drift;

			int tw = 120 + (isin((t * (4 + (i % 5)) + i * 211) & (WAVE_FULL - 1))
				* 135 >> 12);
			if (tw < 0) tw = 0;

			uint32_t c = scaleColor(gp0_rgb(235, 205, 150), tw);
			int size = (i % 7 == 0) ? 3 : ((i % 3 == 0) ? 2 : 1);
			point(chain, bx, y, size, c, true);
		}
	}

	setBlend(chain, GP0_BLEND_SEMITRANS);
}

/* --- style 7: PS5 Spotlight (sweeping ray + soft bokeh) ----------------- */

static void drawPS5Spotlight(RenderContext *ctx, GPUDMAChain *chain) {
	int w = ctx->screenWidth, h = ctx->screenHeight;
	uint32_t t = xmbFrame;
	uint32_t black = gp0_rgb(0, 0, 0);

	/* Dark, cool blue-grey wash, a touch lighter (foggier) at the bottom. */
	setBlend(chain, GP0_BLEND_SEMITRANS);
	{
		uint32_t top = gp0_rgb( 8, 10, 18);
		uint32_t bot = gp0_rgb(20, 24, 34);
		gouraudQuad(chain,
			0, 0, top,  w, 0, top,
			0, h, bot,  w, h, bot,
			false);
	}

	setBlend(chain, GP0_BLEND_ADD);

	/* Spotlight: fixed origin at top-left, beam foot sweeps left<->right
	 * across the whole screen, so it reads as a rotating spotlight. */
	{
		int ox = 46;                                   /* fixed origin x   */
		int oy = -12;
		int foot = (w / 2)
			+ (isin((t * 2) & (WAVE_FULL - 1)) * (w / 2 + 40) >> 12);
		int pulse = 100 + (isin((t * 5) & (WAVE_FULL - 1)) * 30 >> 12);
		uint32_t ray  = scaleColor(gp0_rgb(160, 132, 60), pulse);  /* gold  */
		uint32_t rayI = scaleColor(gp0_rgb(205, 178, 96), pulse);  /* bright gold */

		gouraudQuad(chain,
			ox - 10, oy, ray,   ox + 10, oy, ray,
			foot - 70, h, black, foot + 70, h, black,
			true);
		gouraudQuad(chain,
			ox - 4, oy, rayI,   ox + 4, oy, rayI,
			foot - 26, h, black, foot + 26, h, black,
			true);
	}

	/* Soft foggy floor glow (faint warm tint). */
	{
		uint32_t glow = gp0_rgb(34, 30, 24);
		gouraudQuad(chain,
			0, h - 46, black, w, h - 46, black,
			0, h,      glow,  w, h,      glow,
			true);
	}

	/* Particles with random-feeling behaviour: mixed speeds, sizes and
	 * colours (hard gold / soft gold / white / pale), plus more out-of-focus
	 * bokeh glows. */
	{
		/* colour palette, biased toward gold */
		uint32_t pal[4];
		pal[0] = gp0_rgb(255, 205,  70);  /* hard golden yellow */
		pal[1] = gp0_rgb(220, 180, 110);  /* soft gold          */
		pal[2] = gp0_rgb(245, 245, 235);  /* near white         */
		pal[3] = gp0_rgb(230, 200, 150);  /* pale gold          */

		/* more, larger, slower bokeh (out of focus) */
		for (int i = 0; i < 10; i++) {
			int spd = 2 + (i % 4);                 /* varied slow drift */
			int bx = (i * 173 + (int) t / spd) % (w + 60) - 30;
			int by = 30 + ((i * 71 + (int) t / (spd + 2)) % (h - 50));
			int tw = 50 + (isin((t * (2 + (i % 3)) + i * 500) & (WAVE_FULL - 1))
				* 40 >> 12);
			if (tw < 0) tw = 0;
			uint32_t col = (i % 3 == 0) ? pal[0] : pal[1];
			nebulaBlob(chain, bx, by, 9 + (i % 4) * 4, scaleColor(col, tw));
		}

		/* crisp sparkles: each gets its own speed, size and palette colour */
		for (int i = 0; i < 28; i++) {
			int spd = 3 + (i * 7) % 6;              /* some slow, some fast */
			int bx = (i * 89 + (int) t / spd) % w;
			int by = 16 + ((i * 53 + (int) t / (spd + 1)) % (h - 26));
			int tw = 120 + (isin((t * (4 + (i % 6)) + i * 211) & (WAVE_FULL - 1))
				* 135 >> 12);
			if (tw < 0) tw = 0;
			uint32_t col = pal[(i * 5) % 4];        /* pseudo-random colour */
			int size = (i % 9 == 0) ? 3 : ((i % 3 == 0) ? 2 : 1);
			point(chain, bx, by, size, scaleColor(col, tw), true);
		}
	}

	setBlend(chain, GP0_BLEND_SEMITRANS);
}

/* --- style 14: Nebula (breathing red nebula + banded rotating planets) --
 *
 * Built with the same untextured-hardware toolkit as the rest of this file.
 * The planets went through a GTE-mesh 3D attempt first, but a hand-rolled
 * low-poly bipyramid turned out impossible to verify without hardware to
 * test on, and it rendered as a faceted diamond instead of a sphere. This
 * version instead uses a technique that is GEOMETRICALLY GUARANTEED to be a
 * perfect circle - a flat 2D filled disc - and fakes the 3D rotation with
 * the classic "spinning globe" parametric trick real 90s demos used:
 *
 *   Each surface feature (a band edge or a storm-spot/crack highlight) has
 *   a longitude angle. Its screen X is cx + cos(longitude + spinPhase)*R,
 *   which naturally SLOWS DOWN and bunches up near the left/right limb and
 *   moves fastest through the centre - exactly how a real rotating sphere's
 *   surface features behave in projection. Brightness/width is also scaled
 *   by that same cosine, so features fade out approaching the edge instead
 *   of popping - both combined read as genuine sideways rotation on a solid
 *   round body, with zero risk of ever rendering as anything but a circle.
 *
 * Elements:
 *   1. Cloud frame: many overlapping nebulaBlob() shells, dark-brown ->
 *      bright-peach, additively layered, each one BREATHING (brightness +
 *      radius pulse together, same trick as drawNebulaMorph's blue nebula
 *      above) on top of its own drift.
 *   2. Planets: solid, opaque, banded discs (Jupiter-style horizontal
 *      bands), each with its own palette/band count/spot style so they
 *      read as different worlds, all real circles, all rotating sideways
 *      via the parametric trick above. The hero lava planet adds a
 *      flickering fire ring; the others drift in wide loops that cover
 *      most of the screen, like the Space Cosmos cubes.
 *   3. Lightning: a jagged bolt that fires periodically and briefly
 *      brightens the whole scene.
 *   4. Starfield: twinkling points scattered throughout, plus occasional
 *      red/orange falling stars (short comet streaks) like the Cosmos
 *      theme's shooting stars, distinct from its permanent looping comets.
 */


/* A solid glowing disc planet in the style of the badge art (image 2): an
 * opaque Gouraud-shaded disc (bright core -> deeper rim, so it reads as a
 * lit sphere) wrapped in a soft additive halo. Cheap (one 12-tri fan + one
 * blob) and GEOMETRICALLY always a perfect circle. A single thin equatorial
 * "ring line" is swept by the spin phase so there's a subtle rotation cue
 * without the cost of a full banded surface. */
static void drawGlowDiscPlanet(
	GPUDMAChain *chain, int cx, int cy, int radius,
	uint32_t coreCol, uint32_t rimCol, uint32_t haloCol, uint32_t spin
) {
	// soft additive halo first (behind the body)
	setBlend(chain, GP0_BLEND_ADD);
	nebulaBlob(chain, cx, cy, radius + radius / 2, haloCol);

	// opaque body: bright core fading to a deeper rim
	setBlend(chain, GP0_BLEND_SEMITRANS);
	const int RINGS = 12;
	int px = cx + radius, py = cy;
	for (int k = 1; k <= RINGS; k++) {
		int a  = (k * (WAVE_FULL / RINGS)) & (WAVE_FULL - 1);
		int nx = cx + (icos(a) * radius >> 12);
		int ny = cy + (isin(a) * radius >> 12);
		gouraudTri(chain, cx, cy, coreCol, px, py, rimCol, nx, ny, rimCol, false);
		px = nx; py = ny;
	}

	// subtle equatorial highlight that sweeps with the spin (rotation cue) -
	// a short bright line whose X rides cos(spin), fading near the limb.
	int c = icos(spin & (WAVE_FULL - 1));
	int sx = cx + (c * (radius * 3 / 5) >> 12);
	int bright = (c + 4096) >> 5;              // 0..256
	if (bright > 40) {
		setBlend(chain, GP0_BLEND_ADD);
		point(chain, sx, cy, 2, scaleColor(rimCol, 128 + bright / 2), true);
	}
}

/* Flickering fire ring around the hero planet's silhouette: several small
 * additive flame blobs at evenly spaced angles, each with its own fast
 * flicker phase so they don't pulse in lockstep, slowly precessing around
 * the rim like real licking flames. */
static void drawFireRing(GPUDMAChain *chain, int px, int py, int radius, uint32_t t) {
	setBlend(chain, GP0_BLEND_ADD);

	// Flames are cheap additive points (embers), not full nebula blobs - the
	// hero planet's halo already supplies the soft glow, and keeping this
	// light is what leaves room for a full menu overlay on the same GPU
	// chain (see the budget note in drawNebulaTheme).
	const int FLAMES = 12;
	int creep = (t * 7) & (WAVE_FULL - 1);   // slow precession around the rim
	for (int i = 0; i < FLAMES; i++) {
		int a = creep + i * (WAVE_FULL / FLAMES);
		int flicker = 140 + (isin((t * (9 + (i % 4)) + i * 700) & (WAVE_FULL - 1))
			* 115 >> 12);
		if (flicker < 0) flicker = 0;

		int rr = radius + 2 + (flicker >> 4);
		int fx = px + (icos(a) * rr >> 12);
		int fy = py + (isin(a) * rr >> 12);

		uint32_t flameCol = (i & 1)
			? scaleColor(gp0_rgb(255, 120, 30), flicker)
			: scaleColor(gp0_rgb(255, 210, 60), flicker);
		point(chain, fx, fy, 2 + (flicker >> 6), flameCol, true);
	}
}

/* --- Nebula 2: a real textured hero planet ------------------------------
 *
 * The sun/lava surface (assets/sun.png, pre-resized to 256x128 and quantized
 * to 8bpp so a photographic gradient texture doesn't band) is mapped onto the
 * hero planet with the classic software-globe trick: build a lat/long grid,
 * project each vertex orthographically (screenX = cos(lat)*sin(lon+spin),
 * screenY = sin(lat), both from the isin/icos LUT already used everywhere
 * else in this file), and sample the equirectangular texture directly at
 * (lon/2pi, lat/pi) - no inverse trig needed because WE choose the grid in
 * (lat,lon) space to begin with. Longitude carries the spin offset for
 * position but NOT for its texture U, so the artwork stays glued to the
 * rotating surface instead of sliding. Only the front hemisphere is emitted
 * (a per-quad depth test), same idea as backface culling.
 *
 * Deliberately a coarse grid (6 latitude bands x 10 longitude segments) -
 * see the chain-budget comment on drawNebulaThemeCommon(): this replaces the
 * cheap flat-colour glow disc, so it has to stay modest or Nebula 2 would
 * overflow the GPU chain the same way the original Nebula crash did.
 */
#define SUN_TEX_VRAM_X 832   // clear of the icon sheets (which end at 832);
                             // these four planet sheets then occupy 832..960
                             // for the FULL height of VRAM - see icon.c's
                             // PAD_GLYPH_VRAM_X note before reusing that range
#define SUN_TEX_VRAM_Y   0
#define SUN_TEX_CLUT_X   0   // spare VRAM below the framebuffers (y >= 240)
#define SUN_TEX_CLUT_Y 241
#define SUN_TEX_W      256
#define SUN_TEX_H      128

extern const uint8_t sunTextureData[], sunPaletteData[];
static TextureInfo sunTex;

// Lava/Earth/Moon planet textures for Nebula 3's textured roaming planets.
// 256x128 at 4bpp: 64 VRAM columns each (four 4bpp texels per 16-bit VRAM
// word), stacked at the same proven-safe X column as the sun, one 128-row
// band each, filling the rest of that column down to VRAM's actual 512-row
// limit (0-128 sun, 128-256, 256-384, 384-512). CLUTs stack immediately
// below the sun's CLUT row, same "spare space below the framebuffers" spot -
// 16 entries each now rather than 256, so they no longer span the row.
//
// These three were 8bpp until main RAM ran short. At 256x128 an 8bpp texture
// costs 32 KB + a 512-byte CLUT in the executable; at 4bpp it is 16 KB + 32
// bytes. Three of them saves ~50 KB of RAM. The source PNGs were requantized
// to 16 colours with Floyd-Steinberg dithering, because tools/convertImage.py
// rejects anything above the colour limit rather than quantizing it.
//
// The sun stays at 8bpp deliberately: it is the hero object, drawn far larger
// than the roaming planets, and banding shows on it.
#define LAVA_TEX_VRAM_X  832
#define LAVA_TEX_VRAM_Y  128
#define LAVA_TEX_CLUT_X    0
#define LAVA_TEX_CLUT_Y  242

#define EARTH_TEX_VRAM_X 832
#define EARTH_TEX_VRAM_Y 256
#define EARTH_TEX_CLUT_X   0
#define EARTH_TEX_CLUT_Y 243

#define MOON_TEX_VRAM_X  832
#define MOON_TEX_VRAM_Y  384
#define MOON_TEX_CLUT_X    0
#define MOON_TEX_CLUT_Y  244

extern const uint8_t lavaTextureData[],  lavaPaletteData[];
extern const uint8_t earthTextureData[], earthPaletteData[];
extern const uint8_t moonTextureData[],  moonPaletteData[];
static TextureInfo lavaTex, earthTex, moonTex;

void initNebulaTexture(RenderContext *ctx) {
	(void) ctx;
	uploadIndexedTexture(
		&sunTex, sunTextureData, sunPaletteData,
		SUN_TEX_VRAM_X, SUN_TEX_VRAM_Y, SUN_TEX_CLUT_X, SUN_TEX_CLUT_Y,
		SUN_TEX_W, SUN_TEX_H, GP0_COLOR_8BPP
	);
	uploadIndexedTexture(
		&lavaTex, lavaTextureData, lavaPaletteData,
		LAVA_TEX_VRAM_X, LAVA_TEX_VRAM_Y, LAVA_TEX_CLUT_X, LAVA_TEX_CLUT_Y,
		SUN_TEX_W, SUN_TEX_H, GP0_COLOR_4BPP
	);
	uploadIndexedTexture(
		&earthTex, earthTextureData, earthPaletteData,
		EARTH_TEX_VRAM_X, EARTH_TEX_VRAM_Y, EARTH_TEX_CLUT_X, EARTH_TEX_CLUT_Y,
		SUN_TEX_W, SUN_TEX_H, GP0_COLOR_4BPP
	);
	uploadIndexedTexture(
		&moonTex, moonTextureData, moonPaletteData,
		MOON_TEX_VRAM_X, MOON_TEX_VRAM_Y, MOON_TEX_CLUT_X, MOON_TEX_CLUT_Y,
		SUN_TEX_W, SUN_TEX_H, GP0_COLOR_4BPP
	);
}

#define SUN_LAT_DIV 5    // 6 latitude lines, -90..+90
#define SUN_LON_DIV 8    // 8 longitude segments around the equator

// Coarser grid for the small roaming planets - they're drawn much smaller
// on screen than the hero, so fewer segments are needed to read as round,
// which keeps 3 extra textured spheres affordable on the same GPU chain
// budget that already caused one crash in this file's history (see the
// note on drawNebulaThemeCommon()).
// NOTE: an earlier revision used a much coarser grid here (3 latitude x 6
// longitude) on the theory that small on-screen planets need fewer segments.
// That was wrong - on real hardware/screenshot it rendered as a faceted
// hexagonal gem, not a sphere, because too few longitude segments makes the
// SILHOUETTE itself polygonal, not just the surface shading. The hero
// planet's grid (SUN_LAT_DIV/SUN_LON_DIV, 5x8) is what's actually
// proven to read as round, so the roaming textured planets now reuse it
// directly instead of a separate, untested "cheaper" constant.

static void drawTexturedPlanet(
	GPUDMAChain *chain, int cx, int cy, int radius, uint32_t spin,
	uint32_t modColor, const TextureInfo *tex, int latDiv, int lonDiv
) {
	// Re-select this texture's page each call, since text/other themes'
	// primitives change the bound page in between.
	uint32_t *page = allocateGP0Packet(chain, 1);
	page[0] = gp0_setPage(tex->page, false, false);

	// Precompute each latitude ring's vertical offset and horizontal radius
	// once (shared by every longitude segment at that ring). Fixed-size
	// arrays sized for the coarsest caller (SUN_LAT_DIV); latDiv is always
	// <= that.
	int ringY[SUN_LAT_DIV + 1], ringR[SUN_LAT_DIV + 1], ringV[SUN_LAT_DIV + 1];
	for (int i = 0; i <= latDiv; i++) {
		int lat = -1024 + (i * 2048) / latDiv;   // -1024..+1024 == -90..+90 deg
		ringY[i] = (isin(lat) * radius) >> 12;
		ringR[i] = (icos(lat) * radius) >> 12;
		ringV[i] = (tex->height * i) / latDiv;
	}

	for (int j = 0; j < lonDiv; j++) {
		// Material (fixed, non-spinning) longitude for the texture U - this
		// is what keeps the artwork attached to the surface as it rotates.
		int lon0 = (j       * WAVE_FULL) / lonDiv;
		int lon1 = ((j + 1) * WAVE_FULL) / lonDiv;
		// U spans 0..width-1 (never exactly `width`) - gp0_uv() truncates U
		// to 8 bits, so the old `tex->u + width` formula hit exactly
		// `width` on the final segment, wrapped byte-truncated to 0, and the
		// GPU linearly interpolated U from ~230 backward down to 0 across
		// that one quad: the whole texture smeared into a single sliver,
		// which is exactly the dark seam line down the middle of the
		// planet. Stopping one texel short of the wrap avoids it (a
		// negligible 1-texel seam instead of a corrupted quad).
		int u0 = tex->u + ((tex->width - 1) * j)       / lonDiv;
		int u1 = tex->u + ((tex->width - 1) * (j + 1)) / lonDiv;

		// Displayed longitude includes the spin - this is what actually
		// moves on screen frame to frame. isin()/icos() wrap arbitrary
		// integer input internally, so these are deliberately left
		// unmasked - averaging two already-masked angles across the
		// 0/4096 seam would give a wildly wrong midpoint once per
		// rotation; averaging the raw values first and letting the trig
		// functions wrap is safe everywhere.
		int disp0 = lon0 + (int) spin;
		int disp1 = lon1 + (int) spin;

		// Coarse per-column visibility test: skip this whole longitude
		// strip if its midpoint faces away from the viewer.
		int mid = (disp0 + disp1) / 2;
		if (isin(mid) < -1500)
			continue;

		int cosd0 = icos(disp0), sind0 = isin(disp0);
		int cosd1 = icos(disp1), sind1 = isin(disp1);

		for (int i = 0; i < latDiv; i++) {
			int x00 = cx + (ringR[i]   * cosd0 >> 12), y00 = cy - ringY[i];
			int x10 = cx + (ringR[i]   * cosd1 >> 12), y10 = cy - ringY[i];
			int x01 = cx + (ringR[i+1] * cosd0 >> 12), y01 = cy - ringY[i+1];
			int x11 = cx + (ringR[i+1] * cosd1 >> 12), y11 = cy - ringY[i+1];
			(void) sind0; (void) sind1;   // depth already handled by the column cull above

			int v0 = tex->v + ringV[i], v1 = tex->v + ringV[i + 1];

			uint32_t *ptr = allocateGP0Packet(chain, 9);
			ptr[0] = modColor | gp0_shadedQuad(false, true, false);
			ptr[1] = gp0_xy(x00, y00);
			ptr[2] = gp0_uv(u0, v0, tex->clut);
			ptr[3] = gp0_xy(x10, y10);
			ptr[4] = gp0_uv(u1, v0, tex->page);
			ptr[5] = gp0_xy(x01, y01);
			ptr[6] = gp0_uv(u0, v1, 0);
			ptr[7] = gp0_xy(x11, y11);
			ptr[8] = gp0_uv(u1, v1, 0);
		}
	}
}

/* Jagged lightning bolt: a chain of connected bright segments descending
 * through the cloud layer, forking once partway down. Fires periodically
 * from drawNebulaThemeCommon()'s timer. */
static void drawLightningBolt(GPUDMAChain *chain, int x0, int y0, int y1, uint32_t seed) {
	uint32_t white = gp0_rgb(255, 255, 255);
	uint32_t violet = gp0_rgb(150, 120, 255);

	setBlend(chain, GP0_BLEND_ADD);

	int x = x0, y = y0;
	int forkX = x0, forkY = (y0 + y1) / 2;
	const int SEGS = 7;
	for (int i = 0; i < SEGS; i++) {
		// Cheap deterministic pseudo-random jitter from a running seed.
		seed = seed * 1103515245u + 12345u;
		int jitter = (int) ((seed >> 16) & 0x1f) - 16;

		int ny = y0 + ((y1 - y0) * (i + 1)) / SEGS;
		int nx = x + jitter;

		shadedLine(chain, x, y, white, nx, ny, violet, true);
		x = nx; y = ny;
		if (i == SEGS / 2) { forkX = x; forkY = y; }
	}

	// One short branch off the midpoint.
	int bx = forkX + 22, by = forkY + 26;
	shadedLine(chain, forkX, forkY, white, bx, by, violet, true);
}

/* A background planet that roams in a wide loop covering most of the screen
 * (not just drifting near the centre), like the Space Cosmos cubes but with
 * a bigger range since these are cheap glow discs. Each is drawn with
 * drawGlowDiscPlanet() - solid, opaque, always a perfect circle - unless
 * `tex` is set and the caller asks for textured roaming planets (Nebula 3),
 * in which case it's drawn as a small textured sphere instead (same
 * technique as the hero planet, just a coarser/cheaper grid - see
 * ROAM_LAT_DIV/ROAM_LON_DIV). Either way the halo/glow and the loop motion
 * are identical - only the body's surface changes. */
typedef struct {
	int baseX, baseY;      // loop centre
	int rangeX, rangeY;    // how far the loop reaches from centre
	int speedX, speedY, phaseX, phaseY;
	int radius;
	uint32_t coreCol, rimCol, haloCol;
	const TextureInfo *tex;   // NULL = flat glow-disc (unchanged behaviour)
} RoamingPlanet;

static const RoamingPlanet roamingPlanets[] = {
	// bluish planet, wide slow loop across the upper-left half - Earth
	{  80,  60, 110, 70, 1, 1,  200,  900, 20,
	   0x4A8CFF, 0x1E3C8C, 0x502814 /*halo (BGR-ish additive warm)*/, &earthTex },
	// reddish planet, wide loop across the right half - Lava
	{ 230,  90,  95, 60, 1, 2, 1500,  300, 16,
	   0x4060E0, 0x14206E, 0x201060, &lavaTex },
	// Moon - was tiny and far from the hero; now orbits close around it
	// (the hero sits at roughly cx+30, cy+55) and is sized to actually
	// read clearly instead of disappearing at radius 8.
	{ 205, 195,  55, 35, 2, 3,  700, 2000, 22,
	   0x2A2A38, 0x101018, 0x101014, &moonTex },
	// second small dark moon - unchanged flat glow-disc
	{ 250, 120,  40, 26, 3, 2, 1900,  500,  6,
	   0x2A2A38, 0x101018, 0x101014, NULL },
};
#define ROAMING_COUNT ((int) (sizeof(roamingPlanets) / sizeof(roamingPlanets[0])))

/* Several concurrent falling/shooting stars: short, fading red/orange
 * streaks that appear at random spots, arc a short diagonal, and vanish -
 * distinct from any permanent looping comets. A small fixed pool is cycled
 * so a few can be on screen at once without unbounded state. Cheap: at most
 * FALLING_STARS * (1 line + 1 point) packets per frame. */
#define FALLING_STARS 4
static void updateFallingStars(GPUDMAChain *chain, int w, int h, uint32_t t) {
	static int      active[FALLING_STARS]   = { 0 };
	static int      sx[FALLING_STARS], sy[FALLING_STARS];
	static int      dx[FALLING_STARS], dy[FALLING_STARS];
	static int      life[FALLING_STARS], maxLife[FALLING_STARS];
	static int      cooldown[FALLING_STARS] = { 20, 90, 160, 240 };
	static uint32_t seed = 777;

	setBlend(chain, GP0_BLEND_ADD);
	for (int i = 0; i < FALLING_STARS; i++) {
		if (!active[i]) {
			if (cooldown[i] > 0) { cooldown[i]--; continue; }
			seed = seed * 1103515245u + 12345u;
			sx[i] = (int) (seed % (uint32_t) w);
			sy[i] = (int) ((seed >> 8) % (uint32_t) (h / 2));
			dx[i] = 3 + (int) ((seed >> 16) % 4);
			dy[i] = 2 + (int) ((seed >> 20) % 3);
			maxLife[i] = 16 + (int) ((seed >> 24) % 12);
			life[i] = maxLife[i];
			active[i] = 1;
			continue;
		}

		int hx = sx[i] + dx[i] * (maxLife[i] - life[i]);
		int hy = sy[i] + dy[i] * (maxLife[i] - life[i]);
		int tx = hx - dx[i] * 5, ty = hy - dy[i] * 5;
		int b = 256 * life[i] / maxLife[i];
		uint32_t head = scaleColor(gp0_rgb(255, 220, 160), b);
		uint32_t tail = scaleColor(gp0_rgb(255, 90, 30), b / 3);
		shadedLine(chain, tx, ty, tail, hx, hy, head, true);
		point(chain, hx, hy, 2, head, true);

		life[i]--;
		if (life[i] <= 0 || hx > w + 20 || hy > h + 20) {
			active[i] = 0;
			seed = seed * 1103515245u + 12345u;
			cooldown[i] = 60 + (int) ((seed >> 8) % 240);   // next in ~1-5s
		}
	}
	(void) t;
}

static void drawNebulaThemeCommon(
	RenderContext *ctx, GPUDMAChain *chain, bool enhancedFX,
	bool texturedRoamingPlanets
) {
	// The heroMode parameter this used to take is gone - it selected
	// between a flat glow-disc hero (the original "Nebula" theme) and a
	// lit Gouraud sphere with a day/night terminator ("Nebula 4"). Both
	// were removed (keeping just Nebula 2 and Nebula 3), and both
	// remaining callers already always passed heroMode 1 (the textured
	// lat/long sphere), so the hero is now unconditionally that - see
	// drawTexturedPlanet().
	//
	// enhancedFX: the extra tricks Nebula 3 adds on top of Nebula 2 -
	// writhing corona flares (nebulaBlobWobbly instead of nebulaBlob), a
	// tight additive atmosphere ring hugging the hero body (Gemini's
	// rim-lighting trick), and depth-based starfield parallax. Deliberately
	// kept to a small, bounded primitive-count addition (see the
	// chain-budget comment below) - this file already crashed once from
	// overflowing that budget.
	//
	// texturedRoamingPlanets: Nebula 3 only - three of the five roaming
	// planets (see roamingPlanets[]) get real Earth/Lava/Moon textures
	// instead of a flat colour, at a coarser (cheaper) grid than the hero
	// planet since they're much smaller on screen. Glow and motion are
	// unchanged either way.
	int w = ctx->screenWidth, h = ctx->screenHeight;
	int cx = w / 2, cy = h / 2;
	uint32_t t = xmbFrame;

	// IMPORTANT - chain budget: this background is drawn by renderMenu()
	// BEFORE it draws a full menu of items on top of the SAME GPU DMA chain
	// (GPU_CHAIN_BUFFER_SIZE = 4096 words). The classic RAM/VRAM tester menu
	// is one of the longest menus in the app, so this theme must leave plenty
	// of headroom or allocateGP0Packet()'s overflow assert fires and the app
	// hard-crashes - which is exactly what "only Nebula crashes the RAM
	// tester" was. Every primitive here is deliberately kept cheap (glow-disc
	// planets instead of banded fans, blob/star counts trimmed) so the whole
	// scene stays well under ~1800 words, leaving 2000+ for the menu overlay.

	// Global flash multiplier: bumped for a few frames right after a bolt
	// fires, then decays back to 256 (100%) - the "environmental flash" trick.
	static int flashLevel   = 256;
	static int boltCooldown = 90;
	static uint32_t boltSeed = 12345;

	if (boltCooldown <= 0) {
		flashLevel   = 336;
		boltCooldown = 110 + (int) (t % 70);
		boltSeed     = t * 2654435761u;
	} else {
		boltCooldown--;
	}
	if (flashLevel > 256)
		flashLevel -= 20;
	if (flashLevel < 256)
		flashLevel = 256;

	// --- deep backdrop: near-black fading to a dark ember red ------------
	setBlend(chain, GP0_BLEND_SEMITRANS);
	{
		uint32_t top = gp0_rgb(6, 2, 2);
		uint32_t bot = scaleColor(gp0_rgb(40, 10, 6), flashLevel);
		gouraudQuad(chain,
			0, 0, top,  w, 0, top,
			0, h, bot,  w, h, bot,
			false);
	}

	// --- starfield: scattered, strongly twinkling ------------------------
	// Bigger twinkle swing (near-off to full bright) so they clearly sparkle
	// rather than just shimmering slightly.
	setBlend(chain, GP0_BLEND_ADD);
	for (int i = 0; i < 34; i++) {
		uint32_t h1 = (uint32_t) i * 2654435761u;
		int sx, sy, size, tw;

		if (enhancedFX) {
			// Gemini's parallax-tile trick: give each star a depth layer
			// (1 near .. 4 far) and scroll it sideways at a speed and
			// brightness/size that fall off with depth - the classic
			// multi-layer parallax cue - instead of a fixed twinkling
			// position. Same 34-point budget as the plain version below,
			// just a different formula for where each point lands.
			int depth  = 1 + (int) ((h1 >> 4) % 4u);
			int baseX  = (int) ((h1 >> 8)  % (uint32_t) w);
			int baseY  = (int) ((h1 >> 20) % (uint32_t) (h - 20));
			int scroll = (int) (t * (uint32_t) (5 / depth + 1)) % w;
			sx = (baseX - scroll + w * 4) % w;
			sy = baseY;

			int baseBright = 240 / depth;   // 240, 120, 80, 60 by layer
			tw = baseBright + (isin((t * (3 + (i % 6)) + i * 311)
				& (WAVE_FULL - 1)) * (baseBright / 3) >> 12);
			size = (depth <= 1) ? 2 : 1;
		} else {
			sx = (int) ((h1 >> 8) % (uint32_t) w);
			sy = (int) ((h1 >> 20) % (uint32_t) (h - 20));
			tw = 20 + (isin((t * (3 + (i % 6)) + i * 311) & (WAVE_FULL - 1))
				* 235 >> 12);
			size = (i % 9 == 0) ? 2 : 1;
		}
		if (tw < 0) tw = 0;
		point(chain, sx, sy, size, scaleColor(gp0_rgb(255, 224, 205), tw), true);
	}

	// --- shooting stars: several independent red/orange streaks ----------
	updateFallingStars(chain, w, h, t);

	// --- lightning, occasional flash through the gas ---------------------
	if (flashLevel > 256)
		drawLightningBolt(chain, cx - 30 + (int) (t % 40), 30, h - 70, boltSeed);

	// --- roaming background planets: cheap glow discs, roaming widely ----
	for (int i = 0; i < ROAMING_COUNT; i++) {
		const RoamingPlanet *p = &roamingPlanets[i];
		int ax = ((int) t * p->speedX + p->phaseX) & (WAVE_FULL - 1);
		int ay = ((int) t * p->speedY + p->phaseY) & (WAVE_FULL - 1);
		int px = p->baseX + (icos(ax) * p->rangeX >> 12);
		int py = p->baseY + (isin(ay) * p->rangeY >> 12);
		int spin = (int) t * (3 + (i % 3)) + p->phaseX;

		if (texturedRoamingPlanets && p->tex) {
			// Same halo the flat version draws internally, kept identical
			// so "glow and movement stay the same" as asked.
			setBlend(chain, GP0_BLEND_ADD);
			nebulaBlob(chain, px, py, p->radius + p->radius / 2, p->haloCol);
			setBlend(chain, GP0_BLEND_SEMITRANS);

			uint32_t mod = gp0_rgb(128, 128, 128);   // neutral - texture's own colours
			drawTexturedPlanet(chain, px, py, p->radius, (uint32_t) spin, mod,
				p->tex, SUN_LAT_DIV, SUN_LON_DIV);
		} else {
			drawGlowDiscPlanet(chain, px, py, p->radius,
				p->coreCol, p->rimCol, p->haloCol, (uint32_t) spin);
		}
	}

	// --- hero planet: the glowing yellow disc from the badge art (image 2),
	// pulsing, moved down in frame, with the flickering fire ring -----------
	{
		int pulse = 190 + (isin((t * 5) & (WAVE_FULL - 1)) * 66 >> 12);
		int lit   = pulse * flashLevel >> 8;

		int hx = cx + 30, hy = cy + 55, radius = 46;
		int spin = (int) t * 4;

		uint32_t halo = scaleColor(gp0_rgb(255, 90, 30), 130);   // was 60 - more glow

		// Two extra layered corona passes UNDER the body, on top of the
		// halo above - a broad dim outer glow plus a tight bright inner
		// one, the same "layered bloom" trick real light blooms use. Pure
		// additive, so stacking them just makes the glow richer, not
		// muddier. enhancedFX swaps in nebulaBlobWobbly() for a writhing,
		// flare-like edge instead of a perfectly smooth glow - identical
		// primitive count either way, see nebulaBlobWobbly()'s comment.
		setBlend(chain, GP0_BLEND_ADD);
		int glowPulse = pulse * flashLevel >> 8;
		uint32_t outerGlow = scaleColor(gp0_rgb(255, 120, 40), 90 * glowPulse >> 8);
		uint32_t innerGlow = scaleColor(gp0_rgb(255, 200, 120), 170 * glowPulse >> 8);
		if (enhancedFX) {
			nebulaBlobWobbly(chain, hx, hy, radius * 2,     outerGlow, t, radius / 6);
			nebulaBlobWobbly(chain, hx, hy, radius * 3 / 2, innerGlow, t, radius / 8);
		} else {
			nebulaBlob(chain, hx, hy, radius * 2,     outerGlow);
			nebulaBlob(chain, hx, hy, radius * 3 / 2, innerGlow);
		}

		// The textured body draws only the opaque surface, so the same
		// soft additive glow the flat glow-disc hero used to draw
		// internally is added here explicitly to match.
		setBlend(chain, GP0_BLEND_ADD);
		nebulaBlob(chain, hx, hy, radius + radius / 2, halo);

		// Real textured sphere (Nebula 2/3) - modulation near-neutral so
		// the surface art reads at roughly its own colours, just lit by
		// the same pulse used elsewhere.
		uint32_t mod = scaleColor(gp0_rgb(128, 128, 128), lit);
		drawTexturedPlanet(chain, hx, hy, radius, (uint32_t) spin, mod,
			&sunTex, SUN_LAT_DIV, SUN_LON_DIV);

		if (enhancedFX) {
			// Atmospheric rim halo (Gemini's planet-atmosphere trick): a
			// thin additive ring hugging the body itself, distinct from the
			// two broad corona blooms above which sit much further out.
			// Hero only, to keep this addition small and bounded against
			// the chain budget - see the note at the top of this function.
			setBlend(chain, GP0_BLEND_ADD);
			uint32_t atmosphere = scaleColor(gp0_rgb(255, 165, 95), 70 * glowPulse >> 8);
			nebulaBlob(chain, hx, hy, radius + radius / 4, atmosphere);
		}

		drawFireRing(chain, hx, hy, radius, t);
	}

	// --- breathing nebula cloud frame (trimmed count for chain budget) ---
	setBlend(chain, GP0_BLEND_ADD);
	{
		int breathe = 100 + (isin((t * 4) & (WAVE_FULL - 1)) * 40 >> 12);
		int rp = breathe - 100;
		int glow = breathe * flashLevel >> 8;

		uint32_t deep  = scaleColor(gp0_rgb(60,  16,  6), glow);
		uint32_t peach = scaleColor(gp0_rgb(255, 120, 30), glow);
		uint32_t mid   = scaleColor(gp0_rgb(200,  60, 16), glow);
		uint32_t ember = scaleColor(gp0_rgb(150,  35, 14), glow);

		// 8 scattered drifting clumps (was 13) - still reads as a continuous
		// wispy mass, far cheaper on the chain.
		for (int i = 0; i < 8; i++) {
			int bx0 = (i * 227 + 31) % w;
			int by0 = (i * 131 + 17) % h;
			int spd = 2 + (i % 3);
			int phase = i * 733;
			int dx = isin((t * spd + phase) & (WAVE_FULL - 1)) * (8 + (i % 5) * 3) >> 12;
			int dy = icos((t * (spd + 1) + phase) & (WAVE_FULL - 1)) * (6 + (i % 4) * 2) >> 12;
			int rad = 30 + (i % 5) * 12 + rp / 3;
			uint32_t col = (i % 3 == 0) ? peach : ((i % 3 == 1) ? mid : ember);
			nebulaBlob(chain, bx0 + dx, by0 + dy, rad, col);
		}

		int d0x = isin((t * 2) & (WAVE_FULL - 1)) * 10 >> 12;
		int d0y = icos((t * 3) & (WAVE_FULL - 1)) *  6 >> 12;
		nebulaBlob(chain, 40 + d0x,      50 + d0y,      110 + rp / 3, deep);
		nebulaBlob(chain, w - 50 - d0y,  40 - d0x,       95 + rp / 3, deep);
		nebulaBlob(chain, w / 2 + d0y,   h - 30 - d0x,  100 + rp / 3, deep);

		// bright warm knots scattered through the cloud (10, was 16)
		for (int i = 0; i < 10; i++) {
			int kx = (i * 173 + 59) % w;
			int ky = (i * 97  + 23) % h;
			int tw = 130 + (isin((t * (3 + (i % 5)) + i * 421) & (WAVE_FULL - 1))
				* 125 >> 12);
			if (tw < 0) tw = 0;
			point(chain, kx, ky, (i % 5 == 0) ? 2 : 1,
				scaleColor(gp0_rgb(255, 170, 110), tw), true);
		}

		int d1x = icos((t * 2 + 700) & (WAVE_FULL - 1)) * 14 >> 12;
		int d1y = isin((t * 2 + 700) & (WAVE_FULL - 1)) *  8 >> 12;
		nebulaBlob(chain, cx - 100 + d1x, cy + 40 - d1y,  54 + rp / 2, peach);
		nebulaBlob(chain, cx + 110 - d0x, cy - 60 + d0y,  50 + rp / 2, peach);
	}

	setBlend(chain, GP0_BLEND_SEMITRANS);
}

// Nebula 2 - the hero planet is a real lat/long-textured sphere from
// assets/sun.png (see drawTexturedPlanet). ("Nebula" - the original
// flat glow-disc-hero version - was removed; Nebula 2 and 3 covered
// everything it did and more.)
static void drawNebula2Theme(RenderContext *ctx, GPUDMAChain *chain) {
	drawNebulaThemeCommon(ctx, chain, false, false);
}

// Nebula 3 - the same textured, rotating hero planet as Nebula 2, plus the
// enhancedFX pass: writhing corona flares, a tight atmosphere ring on the
// hero, depth-based starfield parallax, and textured roaming planets.
// Everything else (cloud frame, lightning, falling stars) is unchanged.
static void drawNebula3Theme(RenderContext *ctx, GPUDMAChain *chain) {
	drawNebulaThemeCommon(ctx, chain, true, true);
}

/* --- shared: rotating GTE PS-logo model ----------------------------------
 *
 * A user-supplied GLB (Blender Grease Pencil export, converted offline to
 * psLogoVertices[] / psLogoFaces[] - see ps_logo_model.h). Its colours
 * needed no guessing: the GLB ships a flat-colour material variant whose 4
 * materials are already exactly red/yellow/teal/blue, matching the
 * reference photo directly - see ps_logo_model.h for the details.
 *
 * Used by both the TEST logo theme (full-screen, centred) and PS4 v2
 * (small, fixed in the bottom-right corner) - drawPSLogoModel() below only
 * handles the rotation and draw; the caller is responsible for the camera
 * setup (TRX/TRY/TRZ, OFX/OFY, H) so it can be positioned/sized differently
 * in each.
 *
 * Rotation is a single continuous spin around one axis - the same "just
 * turn in place" motion the roaming/hero planets use elsewhere in this file
 * (their `spin` value feeds one longitude rotation, never a multi-axis
 * tumble) - rather than model_test.c's camera-orbit-style yaw+pitch+lay-flat
 * combination. The model itself is genuinely flat (see ps_logo_model.h),
 * lying face-up in its native GLB orientation, so a fixed 90-degree tilt
 * (STAND_UP_ROLL below) is applied once, before the spin, to stand it up
 * facing the camera.
 *
 * Order matters for that composition and was backwards in an earlier
 * revision: the GTE accumulates RT = firstCall * secondCall, and
 * v' = RT*v = firstCall*(secondCall*v) - so the SECOND call is what
 * actually applies first/innermost to the raw model. Calling the tilt first
 * and the spin second meant the spin was happening in the model's original
 * lying-flat frame, and only the constant tilt got applied afterward,
 * rigidly re-angling the whole already-spinning result - rotating around
 * the wrong axis instead of turning cleanly like a turntable. Calling spin
 * first and tilt second makes the tilt apply innermost (stand the model up
 * first) and the spin apply outermost (turn the now-standing model around
 * its own vertical axis) - the order actually wanted.
 *
 * STAND_UP_ROLL is negative: positive 90 degrees put the red band facing
 * the bottom of the screen instead of the top: flipping the sign tips it
 * the other way.
 *
 * The one real difference from model_test.c is *how* draw order is
 * resolved. That screen owns the whole frame and uses a genuine hardware
 * ordering table (GPU-side linked lists per depth bucket, cleared via OTC
 * DMA) because nothing else shares its GPU chain. A theme doesn't have that
 * luxury - it has to append into the SAME chain the menu overlay is about
 * to draw into, in plain call order, with no per-primitive depth test. So
 * instead of a real OT, every face is GTE-transformed first into a small
 * static array (this model only has 328 triangles - cheap), bucketed by
 * depth using the same AVSZ3/OTZ mechanism the hardware OT would have used,
 * and then emitted back-to-front by scanning that array once per bucket -
 * a software painter's algorithm standing in for the OT's linked lists.
 * Backface culling (GTE_CMD_NCLIP) removes the faces that don't need
 * drawing at all first, same as the existing Cosmos 3D cubes above.
 */

/*
 * Depth buckets for the software painter's-algorithm sort, and the size of
 * the scratch arrays it needs.
 *
 * 256 rather than 64: the sort key is now rescaled to the frame's own depth
 * range (see drawPSLogoFaces()), so every bucket earns its keep instead of
 * the whole model piling into two or three of them. The sort is a counting
 * sort, so the extra buckets cost one pass over an array, not per-face work.
 */
#define PS_LOGO_OT_SIZE  256

// Largest face count of any model drawn through drawPSLogoFaces().
#define PS_LOGO_MAX2(a, b) (((a) > (b)) ? (a) : (b))
#define PS_LOGO_MAX_FACES              \
	PS_LOGO_MAX2(PS_LOGO_FACE_COUNT,   \
	PS_LOGO_MAX2(PS_LOGO_BIOS_FACE_COUNT, PS_LOGO_BIOS2_FACE_COUNT))

// Confirmed by eye: 180 degrees. The geometry-based guess (0, i.e. no
// tilt) was wrong - the model's own bounding box being tall-in-Y suggested
// it was already standing, but it turned out to be standing upside down.
// Used by both the TEST logo theme and PS4 v2's corner copy.
#define STAND_UP_ROLL 2048

// Colour is kept as 3 raw bytes rather than a pre-packed 32-bit GP0 word:
// reconstructing gp0_rgb() at draw time is free next to the GTE work already
// happening per face, and .bss here is not free - this project overflowed
// APP_RAM once the 3rd BGM track pushed total footprint over the 2MB
// console's budget; see CMakeLists.txt/assets for the actual numbers.
//
// The depth is a full int32 and not a bucket index, because the bucket is now
// derived from the frame's own min/max - see drawPSLogoFaces().
typedef struct {
	uint32_t xy0, xy1, xy2;
	uint8_t  r, g, b, pad;
	int32_t  z;
} PSLogoDrawFace;

// Draws the PS logo model, spinning, at whatever camera/projection the
// caller has already configured (TRX/TRY/TRZ, OFX/OFY, H, ZSF3/ZSF4 for
// PS_LOGO_OT_SIZE buckets). `roll` is the fixed stand-up tilt (in the same
// 4096-units-per-circle scale as isin()/icos()/WAVE_FULL) applied once
// before the continuous spin - callers pass STAND_UP_ROLL normally, or a
// different value to test an alternate tilt. Returns the current yaw angle
// in raw units (0..4095) so callers that want the debug degree readout
// don't have to recompute it themselves.
/*
 * As drawPSLogoModel() below, but drawing a caller-chosen model at an explicit
 * pose and brightness rather than psLogoVertices[] free-spinning at full
 * brightness. `bright` is 0..256 and scales every face colour, which is how
 * the boot intro fades the logo up out of black - the faces are flat-shaded,
 * so scaling their colour IS the fade.
 *
 * MIND THE AXIS NAMES. cosmosRotate()'s parameters are called yaw/pitch/roll
 * but its first turns about Z, its second about Y and its third about X.
 * `spinY` therefore goes in slot 2 and `tiltX` in slot 3, and the tilt is
 * passed second so it lands innermost - see the note above on why the call
 * order looks backwards.
 */
static void drawPSLogoFaces(
	GPUDMAChain *chain,
	const PSLogoVertex *verts, const PSLogoFace *faces, int faceCount,
	int bright
) {
	static PSLogoDrawFace drawFaces[PS_LOGO_MAX_FACES];
	int ndraw = 0;
	int zMin = 0x7fffffff, zMax = -0x7fffffff;

	for (int i = 0; i < faceCount; i++) {
		const PSLogoFace *face = &faces[i];

		gte_loadV0((const GTEVector16 *) &verts[face->v0]);
		gte_loadV1((const GTEVector16 *) &verts[face->v1]);
		gte_loadV2((const GTEVector16 *) &verts[face->v2]);
		gte_command(GTE_CMD_RTPT | GTE_SF);

		gte_command(GTE_CMD_NCLIP);
		if (((int) gte_getDataReg(GTE_MAC0)) <= 0)
			continue;   // backface

		uint32_t xy0 = gte_getDataReg(GTE_SXY0);
		uint32_t xy1 = gte_getDataReg(GTE_SXY1);
		uint32_t xy2 = gte_getDataReg(GTE_SXY2);

		/*
		 * Depth key: AVSZ3's MAC0, NOT its OTZ.
		 *
		 * OTZ is MAC0 >> 12, and with ZSF3 = PS_LOGO_OT_SIZE/3 that shift
		 * throws away almost everything this model needs. At the intro's
		 * camera the whole logo spans a couple of hundred model units in
		 * depth against a camera distance of ~430, so every face's OTZ lands
		 * in about three of the sixty-four buckets. Faces that share a bucket
		 * come out in array order, which is sub-mesh order, so the swoosh
		 * painted over the P wherever they crossed - the stray yellow wedge
		 * on the P's stem.
		 *
		 * MAC0 is the same quantity before the shift, so it keeps the
		 * resolution. The range is rescaled to fit the buckets below, which
		 * also makes this independent of camera distance and pose.
		 */
		gte_command(GTE_CMD_AVSZ3 | GTE_SF);
		int z = (int) gte_getDataReg(GTE_MAC0);

		if (z < zMin) zMin = z;
		if (z > zMax) zMax = z;

		PSLogoDrawFace *df = &drawFaces[ndraw++];
		df->xy0 = xy0;
		df->xy1 = xy1;
		df->xy2 = xy2;
		df->r   = (uint8_t) ((face->r * bright) >> 8);
		df->g   = (uint8_t) ((face->g * bright) >> 8);
		df->b   = (uint8_t) ((face->b * bright) >> 8);
		df->z   = z;
	}

	if (!ndraw)
		return;

	/*
	 * Counting sort, far to near.
	 *
	 * The previous version rescanned the whole face array once per bucket -
	 * fine at 64 buckets, quadratic as soon as there are enough buckets to be
	 * useful. This is two linear passes plus the bucket histogram, so the
	 * resolution is free.
	 */
	int shift = 0;
	int range = zMax - zMin;

	while ((range >> shift) >= PS_LOGO_OT_SIZE)
		shift++;

	static uint16_t counts[PS_LOGO_OT_SIZE + 1];
	static uint16_t order[PS_LOGO_MAX_FACES];

	for (int b = 0; b <= PS_LOGO_OT_SIZE; b++)
		counts[b] = 0;

	for (int i = 0; i < ndraw; i++)
		counts[((zMax - drawFaces[i].z) >> shift) + 1]++;

	for (int b = 0; b < PS_LOGO_OT_SIZE; b++)
		counts[b + 1] += counts[b];

	for (int i = 0; i < ndraw; i++)
		order[counts[(zMax - drawFaces[i].z) >> shift]++] = (uint16_t) i;

	for (int k = 0; k < ndraw; k++) {
		const PSLogoDrawFace *df = &drawFaces[order[k]];

		uint32_t *ptr = allocateGP0Packet(chain, 4);
		ptr[0] = gp0_rgb(df->r, df->g, df->b)
			| gp0_shadedTriangle(false, false, false);
		ptr[1] = df->xy0;
		ptr[2] = df->xy1;
		ptr[3] = df->xy2;
	}
}

static int drawPSLogoModel(GPUDMAChain *chain, uint32_t t, int roll) {
	int yaw = (int) t * 8 & 4095;

	gte_setRotationMatrix(GTE_UNIT, 0, 0,  0, GTE_UNIT, 0,  0, 0, GTE_UNIT);
	cosmosRotate(0, yaw, 0);
	cosmosRotate(0, 0, roll);

	drawPSLogoFaces(chain, psLogoVertices, psLogoFaces, PS_LOGO_FACE_COUNT, 256);
	return yaw;
}

/*
 * The boot sequence's PlayStation screen draws the OTHER model - the real one
 * in ps_logo_bios.h, not the coarse ps_logo_model.h the themes use - posed
 * rather than free-spinning and positioned to match the BIOS screen.
 *
 * The resting pose is baked into that model's vertices (see its header), so
 * all three angles at 0 IS the BIOS orientation. They are offsets from it.
 *
 *   cx, cy   where the model's centre lands on screen, in pixels
 *   camZ     camera distance; smaller is bigger. The posed model is 326 units
 *            wide and setupCosmosGTE() sets H = min(w,h)/2, so 120 at
 *            320x240: camZ 430 gives roughly 110 pixels across.
 *   yaw      turntable, about the vertical Y axis
 *   pitch    flip, about the horizontal X axis
 *   roll     spin in the screen plane, about the depth Z axis
 *   bright   0..256, scales the flat face colours (the fade)
 *
 * Angles are in the 4096-per-circle scale isin() uses, and are applied yaw
 * innermost then pitch then roll - so yaw turns the logo on its own base, and
 * roll then tips the whole result in the screen plane.
 */
/*
 * The two candidate logo models, in menu order.
 *
 * A is the Sketchfab relief - a shallow extrusion of the flat artwork. B is
 * the system-BIOS model, which is genuinely built the way the real screen's
 * logo is: the P standing upright on a swoosh lying flat on the ground plane,
 * rather than everything in one shallow slab.
 */
const char *const xmbIntroLogoNames[XMB_INTRO_LOGO_COUNT] = {
	"A relief",
	"B system BIOS",
};

static const struct {
	const PSLogoVertex *verts;
	const PSLogoFace   *faces;
	int                 faceCount;
} introLogos[XMB_INTRO_LOGO_COUNT] = {
	{ psLogoBiosVertices,  psLogoBiosFaces,  PS_LOGO_BIOS_FACE_COUNT  },
	{ psLogoBios2Vertices, psLogoBios2Faces, PS_LOGO_BIOS2_FACE_COUNT },
};

void xmbDrawIntroPSLogo(
	RenderContext *ctx, int model, int cx, int cy, int camZ,
	int yaw, int pitch, int roll, int bright
) {
	GPUDMAChain *chain = getCurrentChain(ctx);

	if (model < 0 || model >= XMB_INTRO_LOGO_COUNT)
		model = 0;

	setupCosmosGTE(ctx->screenWidth, ctx->screenHeight);
	gte_setControlReg(GTE_ZSF3, PS_LOGO_OT_SIZE / 3);
	gte_setControlReg(GTE_ZSF4, PS_LOGO_OT_SIZE / 4);

	// setupCosmosGTE() centres OFX/OFY; override so the logo can sit where
	// the BIOS puts it rather than at dead centre.
	gte_setControlReg(GTE_OFX, cx << 16);
	gte_setControlReg(GTE_OFY, cy << 16);
	gte_setControlReg(GTE_TRX, 0);
	gte_setControlReg(GTE_TRY, 0);
	gte_setControlReg(GTE_TRZ, camZ);

	setBlend(chain, GP0_BLEND_SEMITRANS);

	// Three separate calls, outermost first: cosmosRotate() accumulates
	// RT = first * second, so the LAST call is the one applied to the raw
	// model. Roll, pitch, yaw therefore means yaw innermost.
	gte_setRotationMatrix(GTE_UNIT, 0, 0,  0, GTE_UNIT, 0,  0, 0, GTE_UNIT);
	if (roll)
		cosmosRotate(roll & 4095, 0, 0);    /* slot 1 turns about Z */
	if (pitch)
		cosmosRotate(0, 0, pitch & 4095);   /* slot 3 turns about X */
	cosmosRotate(0, yaw & 4095, 0);         /* slot 2 turns about Y */

	drawPSLogoFaces(chain, introLogos[model].verts, introLogos[model].faces,
		introLogos[model].faceCount, bright);
}

/* --- style: PS4 (flowing silk ribbons on deep blue) ---------------------
 *
 * Matches the reference PS4 dynamic-wallpaper GIF: a fairly uniform deep
 * royal blue field (not the strong top-to-bottom gradient Aurora above
 * uses), one soft glow patch high in the frame, and several broad, slow,
 * diagonal translucent folds sweeping across it - like backlit silk rather
 * than neon light.
 *
 * Reuses Aurora's exact ribbon-segment trick unchanged (a chain of Gouraud
 * quads, bright core fading to black at both edges, additive blend so black
 * adds nothing and the edge reads as transparent) - only the constants
 * differ: each ribbon's baseline runs diagonally (a per-segment X tilt
 * proportional to Y, on top of the same sine wobble Aurora uses), the cores
 * are dim pale blue-white instead of saturated neon, and everything moves
 * far slower to match the gentle drift in the reference. */

typedef struct {
	int startX, tiltX;      // baseline: x = startX + tiltX * y / h (diagonal)
	int amp, freq, speed;
	int halfW;
	uint32_t color;
} SilkRibbon;

// Shared by drawPS4Theme() and drawPS4V2Theme(): a chain of segmented
// Gouraud quads per ribbon, bright core fading to black at both edges,
// additive blend so black adds nothing and the edge reads as transparent.
// Caller sets the blend mode and picks segment count via `seg` (each
// segment costs 2 quads = 16 words per ribbon).
static void drawSilkRibbons(
	GPUDMAChain *chain, const SilkRibbon *ribs, int count, int h, uint32_t t,
	int seg
) {
	int step = h / seg;
	uint32_t black = gp0_rgb(0, 0, 0);

	for (int r = 0; r < count; r++) {
		const SilkRibbon *R = &ribs[r];
		for (int i = 0; i < seg; i++) {
			int y0 = i * step;
			int y1 = (i == seg - 1) ? h : (i + 1) * step;

			int base0 = R->startX + R->tiltX * y0 / h;
			int base1 = R->startX + R->tiltX * y1 / h;

			int a0 = (i       * R->freq + (int) t * R->speed) & (WAVE_FULL - 1);
			int a1 = ((i + 1) * R->freq + (int) t * R->speed) & (WAVE_FULL - 1);
			int x0 = base0 + (isin(a0) * R->amp >> 12);
			int x1 = base1 + (isin(a1) * R->amp >> 12);

			gouraudQuad(chain,
				x0 - R->halfW, y0, black, x0, y0, R->color,
				x1 - R->halfW, y1, black, x1, y1, R->color,
				true);
			gouraudQuad(chain,
				x0, y0, R->color, x0 + R->halfW, y0, black,
				x1, y1, R->color, x1 + R->halfW, y1, black,
				true);
		}

	}
}

static void drawPS4Theme(RenderContext *ctx, GPUDMAChain *chain) {
	int w = ctx->screenWidth;
	int h = ctx->screenHeight;
	uint32_t t = xmbFrame;

	// Fairly uniform deep royal blue - a gentle gradient, not Aurora's
	// strong navy->bright wash.
	setBlend(chain, GP0_BLEND_SEMITRANS);
	{
		uint32_t top = gp0_rgb(18, 34, 108);
		uint32_t bot = gp0_rgb(28, 50, 132);
		gouraudQuad(chain,
			0, 0, top,  w, 0, top,
			0, h, bot,  w, h, bot,
			false);
	}

	// One soft glow patch high in the frame, matching the subtle brightening
	// visible in the upper-middle of the reference image.
	setBlend(chain, GP0_BLEND_ADD);
	nebulaBlob(chain, w * 3 / 5, h / 6, w / 2,
		gp0_rgb(30, 40, 60));

	SilkRibbon rib[4];
	rib[0] = (SilkRibbon){  40,  90, 22, 5,  6, 70, gp0_rgb(70,  95, 150) };
	rib[1] = (SilkRibbon){ 120, 140, 30, 4,  7, 90, gp0_rgb(90, 120, 175) };
	rib[2] = (SilkRibbon){ 210, -80, 26, 6,  5, 60, gp0_rgb(55,  80, 135) };
	rib[3] = (SilkRibbon){ 300, 120, 34, 3,  8, 55, gp0_rgb(100, 130, 185) };

	setBlend(chain, GP0_BLEND_ADD);
	drawSilkRibbons(chain, rib, 4, h, t, 12);

	setBlend(chain, GP0_BLEND_SEMITRANS);
}

/* --- style: PS4 v2 (drifting outline glyphs: triangle, circle, cross,
 * square) ------------------------------------------------------------------
 *
 * Recreates the classic PS4 "Shapes" dynamic wallpaper: the four PlayStation
 * button glyphs, drawn as thin glowing outlines (not filled), drift slowly
 * around a very dark navy background, each slowly rotating and gently
 * changing depth (a slow near/far breathe that scales size and brightness
 * together, the same "closer = bigger and brighter" cue the Nebula themes'
 * roaming planets already use).
 *
 * All four shapes are generated from the same primitive: N points evenly
 * spaced around a circle (computeRingVerts below), rotated by a per-instance
 * spin angle. What differs is only how consecutive points are connected:
 *   - circle:   N=16 points, connect every adjacent pair (a smooth ring)
 *   - triangle: N=3 points,  connect every adjacent pair
 *   - square:   N=4 points,  connect every adjacent pair
 *   - cross:    N=4 points,  connect 0-2 and 1-3 (the two diagonals)
 * so one small function does the vertex math for all four glyphs.
 *
 * Each glyph is a soft additive glow (nebulaBlob, dim) plus the crisp
 * outline drawn with shadedLine on top - the same two-layer "glow behind,
 * crisp shape on top" trick used throughout this file (e.g. the Nebula
 * hero's corona + body, or the falling stars' head + tail).
 */

typedef enum { GLYPH_CIRCLE, GLYPH_TRIANGLE, GLYPH_SQUARE, GLYPH_CROSS } GlyphType;

static void computeRingVerts(
	int cx, int cy, int radius, int angleOffset, int count, int *xs, int *ys
) {
	for (int k = 0; k < count; k++) {
		int a = angleOffset + (k * WAVE_FULL) / count;
		xs[k] = cx + (icos(a) * radius >> 12);
		ys[k] = cy + (isin(a) * radius >> 12);
	}
}

// bigGlow: true = a full soft nebulaBlob glow behind the shape (cheap enough
// for a handful of large, loosely-drifting glyphs - see drawPS4Theme's
// sibling above). false = a single small additive point instead - all a
// glyph this tiny needs, and the only way "a lot of them on screen" (the
// spray variant below) stays affordable on the same GPU chain budget that
// already caused one crash in this file's history.
static void drawGlyph(
	GPUDMAChain *chain, int cx, int cy, int radius, int angle,
	GlyphType type, uint32_t color, uint32_t glowColor, bool bigGlow
) {
	setBlend(chain, GP0_BLEND_ADD);
	if (bigGlow)
		nebulaBlob(chain, cx, cy, radius * 2, glowColor);
	else
		point(chain, cx, cy, radius, glowColor, true);

	int xs[16], ys[16];
	int count;

	switch (type) {
		case GLYPH_CIRCLE:   count = bigGlow ? 16 : 6; break;
		case GLYPH_CROSS:    count = 4;  break;
		case GLYPH_SQUARE:   count = 4;  break;
		case GLYPH_TRIANGLE: default: count = 3; break;
	}
	computeRingVerts(cx, cy, radius, angle, count, xs, ys);

	setBlend(chain, GP0_BLEND_ADD);
	if (type == GLYPH_CROSS) {
		// Two diagonals of the 4-point square, not the perimeter.
		shadedLine(chain, xs[0], ys[0], color, xs[2], ys[2], color, true);
		shadedLine(chain, xs[1], ys[1], color, xs[3], ys[3], color, true);
	} else {
		for (int k = 0; k < count; k++) {
			int n = (k + 1) % count;
			shadedLine(chain, xs[k], ys[k], color, xs[n], ys[n], color, true);
		}
	}
}

// One roaming glyph instance: wide slow Lissajous-style loop (same shape of
// motion as the Nebula themes' roaming planets), independent rotation
// speed, and a slow depth breathe that scales size/brightness together.
// (Was used by an earlier revision of PS4 v2's loose-drifting layout;
// PS4 v2 now uses the "spray" stream formula in drawPS4V2Theme() instead,
// but drawGlyph()'s bigGlow=true path exists for exactly this kind of
// larger, sparser, more visible glyph if a future variant wants it.)

static void drawPS4V2Theme(RenderContext *ctx, GPUDMAChain *chain) {
	int w = ctx->screenWidth;
	int h = ctx->screenHeight;
	uint32_t t = xmbFrame;

	// Very dark navy, close to black - matches the reference video's
	// background far more than any of the brighter themes above.
	setBlend(chain, GP0_BLEND_SEMITRANS);
	{
		uint32_t top = gp0_rgb(4, 6, 20);
		uint32_t bot = gp0_rgb(10, 14, 34);
		gouraudQuad(chain,
			0, 0, top,  w, 0, top,
			0, h, bot,  w, h, bot,
			false);
	}

	// Depth, matching the reference: a soft glow patch plus a couple of
	// slow, dim, diagonal light-ribbon streaks toward one side (reusing
	// drawSilkRibbons() from the PS4 v1 theme above) - the reference isn't
	// a flat gradient, it has real soft light structure drifting through
	// it. Kept noticeably dimmer/sparser than PS4 v1's own ribbons so it
	// reads as background depth behind the glyphs, not a competing subject.
	setBlend(chain, GP0_BLEND_ADD);
	nebulaBlob(chain, w * 7 / 10, h * 2 / 5, w * 3 / 5,
		gp0_rgb(14, 18, 30));

	SilkRibbon depthRib[2];
	depthRib[0] = (SilkRibbon){ w - 40,  60, 24, 5, 3, 50, gp0_rgb(30, 42, 68) };
	depthRib[1] = (SilkRibbon){ w - 90, -50, 30, 4, 2, 60, gp0_rgb(22, 32, 56) };
	drawSilkRibbons(chain, depthRib, 2, h, t, 6);   // fewer segments than PS4 v1's - see the chain-budget note below

	uint32_t glyphColor = gp0_rgb(150, 185, 235);   // pale blue-white, monochrome like the reference
	uint32_t glowColor  = gp0_rgb(20, 30, 55);

	// A continuous stream of small glyphs "sprayed" in from the top-right
	// corner. Most drift left with the classic fountain arc, decelerating
	// as they go - fast near the spawn point, visibly slower by the middle
	// of the screen, slowest just before they exit at the left edge. A
	// third of them instead splash outward in a random fixed direction from
	// the same spawn area, like droplets scattering off a stream of water,
	// rather than all moving in lockstep to the left.
	//
	// Deliberately stateless (a pure function of the frame counter and
	// particle index, same style as every other effect in this file) rather
	// than a simulated particle pool - each particle's entire lifetime
	// position is one closed-form formula, so there's no per-particle state
	// to keep in sync.
	//
	// Small and cheap per instance is what makes a LOT of them affordable:
	// each one is a single glow point (point(), 3 words) plus a tiny
	// outline (at most 8 short lines for a circle, 32 words) - see
	// drawGlyph()'s bigGlow=false path - rather than the full soft
	// nebulaBlob() glow the loose-drifting variant above uses.
	//
	// CHAIN BUDGET: this theme combined with a text-heavy screen (Console
	// Info was the one that actually crashed on real hardware) was
	// computed at up to ~2900 words for the background alone against the
	// shared 4096-word GPU chain buffer - and allocateGP0Packet()'s only
	// bounds check is a plain assert(), which -DNDEBUG strips out of the
	// release build entirely. An overflow there doesn't fail loudly, it
	// silently corrupts whatever memory sits right after the chain buffer
	// - exactly the "random crash depending on what else is on screen"
	// signature that was reported. SPRAY_COUNT was the single largest
	// contributor (56 particles was over half this theme's total cost by
	// itself) so it's the main thing cut back, along with the depth
	// ribbons' segment count just above.
	#define SPRAY_COUNT    32
	#define SPRAY_SPAWN_X  (w + 60)     // off-screen right
	#define SPRAY_EXIT_X   (-40)        // off-screen left
	#define SPRAY_RANGE    (SPRAY_SPAWN_X - SPRAY_EXIT_X)
	#define SPRAY_TOP_BAND (h * 2 / 5)  // stream particles spawn in the upper-right
	#define SPLASH_MAX_DIST 460         // covers the screen diagonal with margin

	for (int i = 0; i < SPRAY_COUNT; i++) {
		uint32_t h1 = (uint32_t) i * 2654435761u;

		// Depth: 1 (near - big, bright, fast) .. 4 (far - small, dim, slow).
		int depth  = 1 + (int) ((h1 >> 4) % 4u);
		int spin   = (int) t * (4 + (i % 5)) + (int) (h1 >> 2);
		int size   = 3 + (4 - depth) * 2;             // depth 1 -> 9px, depth 4 -> 3px
		int bright = 260 / depth;
		uint32_t col  = scaleColor(glyphColor, bright);
		uint32_t glow = scaleColor(glowColor,  bright);
		GlyphType type = (GlyphType) (i % 4);

		int px, py;

		if (i % 3 == 2) {
			// --- splash: shoots outward in one fixed random direction from
			// near the spawn corner, like a droplet flung off the main
			// stream, instead of joining the leftward flow.
			int angle    = (int) (h1 % (uint32_t) WAVE_FULL);
			int speed    = 3 - (depth / 3);           // was 5-(depth/2), slowed down
			int phase    = (int) ((h1 >> 6) % (uint32_t) SPLASH_MAX_DIST);
			int distance = (int) (((uint32_t) t * (uint32_t) speed + (uint32_t) phase)
				% (uint32_t) SPLASH_MAX_DIST);

			int originX = w - 40 + (int) ((h1 >> 14) % 80u);
			int originY = (int) ((h1 >> 18) % (uint32_t) SPRAY_TOP_BAND);

			px = originX + (icos(angle) * distance >> 12);
			py = originY + (isin(angle) * distance >> 12);
		} else {
			// --- main stream: left-flowing fountain arc, eased so it
			// decelerates the whole way - fast off the spawn point,
			// noticeably slower by mid-screen, slowest right at the exit.
			// Cubic ease-out computed entirely in integers: with q = the
			// remaining *linear* distance, the remaining *eased* distance
			// is q^3 / RANGE^2, so it shrinks quickly at first (still far
			// from the edge - looks fast) and barely at all once q is
			// already small (near the edge - looks slow).
			int speed = 4 - depth / 2;                // 3..2, roughly half the old 6..3 -
			                                           // was still too fast
			int phase = (int) (h1 % (uint32_t) SPRAY_RANGE);
			int raw = (int) (((uint32_t) t * (uint32_t) speed + (uint32_t) phase)
				% (uint32_t) SPRAY_RANGE);

			int q  = SPRAY_RANGE - raw;                       // remaining, linear
			int qe = (q * q / SPRAY_RANGE) * q / SPRAY_RANGE; // remaining, eased (~q^3/RANGE^2)
			int travelEased = SPRAY_RANGE - qe;

			px = SPRAY_SPAWN_X - travelEased;

			int spawnY = (int) ((h1 >> 10) % (uint32_t) SPRAY_TOP_BAND);
			int fall = travelEased * (h * 2 / 3) / SPRAY_RANGE;
			int wobble = isin(((int) t * (3 + (i % 5)) + i * 211)
				& (WAVE_FULL - 1)) * 5 >> 12;
			py = spawnY + fall + wobble;
		}

		drawGlyph(chain, px, py, size, spin, type, col, glow, false);
	}

	// Fixed rotating PS logo model, bottom-right corner - same size and
	// spin as the TEST logo theme, just repositioned via GTE_OFX/OFY
	// instead of driving TRX/TRY (which would also scale its parallax) and
	// with no drift/roaming motion of its own, unlike the spray glyphs
	// above - it just sits in place and turns.
	setupCosmosGTE(w, h);
	gte_setControlReg(GTE_ZSF3, PS_LOGO_OT_SIZE / 3);
	gte_setControlReg(GTE_ZSF4, PS_LOGO_OT_SIZE / 4);
	gte_setControlReg(GTE_TRX, 0);
	gte_setControlReg(GTE_TRY, 0);
	gte_setControlReg(GTE_TRZ, 450);
	gte_setControlReg(GTE_OFX, (w * 78 / 100) << 16);
	gte_setControlReg(GTE_OFY, (h * 78 / 100) << 16);
	drawPSLogoModel(chain, t, STAND_UP_ROLL);

	setBlend(chain, GP0_BLEND_SEMITRANS);
}

/* --- style: TEST logo (rotating GTE PS-logo model on black) ------------- */

// Shared by the main TEST logo theme and its 3 alternate-tilt siblings
// below (see the note on STAND_UP_ROLL) - identical except for which roll
// value gets passed to drawPSLogoModel() and the label on the debug
// readout, so it's obvious on screen which variant is which.
static void drawTestLogoThemeWithRoll(
	RenderContext *ctx, GPUDMAChain *chain, int roll, const char *label
) {
	int w = ctx->screenWidth;
	int h = ctx->screenHeight;
	uint32_t t = xmbFrame;

	// Solid black backdrop, as requested.
	setBlend(chain, GP0_BLEND_SEMITRANS);
	{
		uint32_t black = gp0_rgb(0, 0, 0);
		gouraudQuad(chain,
			0, 0, black,  w, 0, black,
			0, h, black,  w, h, black,
			false);
	}

	setupCosmosGTE(w, h);
	gte_setControlReg(GTE_ZSF3, PS_LOGO_OT_SIZE / 3);
	gte_setControlReg(GTE_ZSF4, PS_LOGO_OT_SIZE / 4);

	// Same camera distance as model_test.c's MODEL_CAM_DIST; no pan needed,
	// the model is already centred on its own bounding-box middle (see
	// ps_logo_model.h's provenance comment). setupCosmosGTE() already put
	// OFX/OFY at screen centre, which is what this theme wants.
	gte_setControlReg(GTE_TRX, 0);
	gte_setControlReg(GTE_TRY, 0);
	gte_setControlReg(GTE_TRZ, 450);

	int yaw = drawPSLogoModel(chain, t, roll);

	// On-screen angle readout in degrees, so which variant is which - and
	// any further adjustment - can be given as exact numbers instead of
	// re-guessing blind each time.
	{
		char buf[48];
		snprintf(buf, sizeof(buf), "%s: roll %d  yaw %d",
			label, roll * 360 / 4096, (yaw * 360) / 4096);
		printString(ctx, 8, h - 14, 0x606060, buf);
	}
}

static void drawTestLogoTheme(RenderContext *ctx, GPUDMAChain *chain) {
	drawTestLogoThemeWithRoll(ctx, chain, STAND_UP_ROLL, "main");
}

/* --- entry point -------------------------------------------------------- */
// A cheap, fixed alternative to the live theme background - a single flat
// quad, no per-theme logic, no GTE, no per-frame primitive count that
// scales with which theme happens to be selected. For screens where
// rendering the full theme alongside whatever the screen itself is doing
// is a real risk rather than just a preference - see ramtester.c and
// ramconfig.c's use of this, and the comment on isHeavyBackgroundUnsafe()
// in ui.c for why.
void drawFlatBackdrop(RenderContext *ctx, uint32_t color) {
	GPUDMAChain *chain = getCurrentChain(ctx);
	setBlend(chain, GP0_BLEND_SEMITRANS);
	gouraudQuad(chain,
		0, 0, color,  ctx->screenWidth, 0, color,
		0, ctx->screenHeight, color,  ctx->screenWidth, ctx->screenHeight, color,
		false);
}

/*
 * Push the wave animation forward by more than the usual one frame.
 *
 * drawXMBBackground() advances xmbFrame by 1 per call, which is the normal
 * idle drift. The boot intro uses this to accelerate the waves without
 * touching the wave code itself: the layers are all functions of xmbFrame,
 * so making time pass faster is the whole effect.
 */
void xmbAdvanceFrames(uint32_t extra) {
	xmbFrame += extra;
}

void drawXMBBackground(RenderContext *ctx) {
	GPUDMAChain *chain = getCurrentChain(ctx);

	if (isBackgroundScrollEnabled())
		xmbFrame++;

	// Follow the theme picked in the menu (live).
	if (xmbThemeIndex < XMB_THEME_COUNT)
		currentStyle = themeStyles[xmbThemeIndex];

	switch (currentStyle) {
		case XMB_BG_GOURAUD_SPARKLE: drawGouraudSparkle(ctx, chain); break;
		case XMB_BG_PARALLAX:        drawParallax(ctx, chain);       break;
		case XMB_BG_AURORA:          drawAurora(ctx, chain);         break;
		case XMB_BG_COSMOS:          drawCosmos(ctx, chain);         break;
		case XMB_BG_COSMOS_3D_PP:    drawCosmos3DPlusPlus(ctx, chain); break;
		case XMB_BG_COSMOS_3D_PP2:   drawCosmos3DPlusPlus2(ctx, chain); break;
		case XMB_BG_GOURAUD_PSP:     drawGouraudPSP(ctx, chain);      break;
		case XMB_BG_PSP_BEND:        drawPSPBend(ctx, chain);         break;
		case XMB_BG_PSP_THIN:        drawPSPThin(ctx, chain);         break;
		case XMB_BG_PS5:             drawPS5Sparkle(ctx, chain);     break;
		case XMB_BG_PS5_SPOTLIGHT:   drawPS5Spotlight(ctx, chain);   break;
		case XMB_BG_NEBULA2:         drawNebula2Theme(ctx, chain);   break;
		case XMB_BG_NEBULA3:         drawNebula3Theme(ctx, chain);   break;
		case XMB_BG_PS4:             drawPS4Theme(ctx, chain);       break;
		case XMB_BG_PS4_V2:          drawPS4V2Theme(ctx, chain);     break;
		case XMB_BG_TEST_LOGO:       drawTestLogoTheme(ctx, chain);  break;
		case XMB_BG_GOURAUD_WAVES:
		default:                     drawGouraudWaves(ctx, chain);   break;
	}
}
