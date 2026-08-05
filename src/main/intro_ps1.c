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
#include "main/defs.h"
#include "main/font.h"
#include "main/intro_ps1.h"
#include "main/renderer.h"
#include "main/sound.h"
#include "main/trig.h"

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
#define SONY_W 160   /* padded; artwork is 160x28 */
#define SONY_H  32
#define SCET_W 128   /* padded; artwork is 122x24 */
#define SCET_H  24
#define PSLG_W  96   /* padded; artwork is 90x82  */
#define PSLG_H  88
#define PSTX_W  80   /* padded; artwork is 73x16  */
#define PSTX_H  16

/* Visible artwork inside each padded texture. */
#define SONY_DW 160
#define SONY_DH  28
#define SCET_DW 122
#define SCET_DH  24
#define PSLG_DW  90
#define PSLG_DH  82
#define PSTX_DW  73
#define PSTX_DH  16

#define INTRO_VRAM_X 960
#define SONY_VRAM_Y   64
#define SCET_VRAM_Y   96
#define PSLG_VRAM_Y  128
#define PSTX_VRAM_Y  224

#define SONY_CLUT_X 752
#define SCET_CLUT_X 768
#define PSLG_CLUT_X 784
#define PSTX_CLUT_X 800
#define INTRO_CLUT_Y 256

static TextureInfo sonyTex, sceTextTex, psLogoTex, psTextTex;

/* Which look to use, chosen from the boot menu. See PS1IntroVariant. */
static int introVariant = PS1_INTRO_CLASSIC;

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

/* ===================================================================== *
 * Variant support
 *
 * Three looks for the SCE screen, chosen from a menu at boot so they can be
 * compared on real hardware in a single build. All three share the timeline,
 * the wordmarks and the PlayStation screen - only the diamond changes.
 * ===================================================================== */

/* Scale a 0xBBGGRR colour by n/256, clamped. */
static uint32_t scaleCol(uint32_t c, int n) {
	uint32_t r = ((c        & 0xff) * n) >> 8;
	uint32_t g = (((c >> 8)  & 0xff) * n) >> 8;
	uint32_t b = (((c >> 16) & 0xff) * n) >> 8;

	if (r > 0xff) r = 0xff;
	if (g > 0xff) g = 0xff;
	if (b > 0xff) b = 0xff;

	return (b << 16) | (g << 8) | r;
}

/* Blended flat quad, for the glass body and the glow rings. */
static void glassQuad(
	RenderContext *ctx,
	int x0, int y0, int x1, int y1,
	int x2, int y2, int x3, int y3,
	uint32_t colour
) {
	GPUDMAChain *chain = getCurrentChain(ctx);
	uint32_t *ptr = allocateGP0Packet(chain, 5);

	ptr[0] = colour | gp0_shadedQuad(false, false, true);
	ptr[1] = gp0_xy(x0, y0);
	ptr[2] = gp0_xy(x1, y1);
	ptr[3] = gp0_xy(x2, y2);
	ptr[4] = gp0_xy(x3, y3);
}

/*
 * Sparks bouncing inside the diamond.
 *
 * A point is inside a rhombus when |dx|/rx + |dy|/ry <= 1, which is the
 * cheapest containment test available - no square roots, no trig. On a hit
 * the velocity component with the larger contribution is flipped, which is a
 * good enough approximation of reflecting off a slanted wall at this size
 * and costs nothing.
 */
#define SPARK_COUNT 14

typedef struct {
	int x, y, vx, vy;
} Spark;

static Spark sparks[SPARK_COUNT];
static bool  sparksReady;

static void resetSparks(int r) {
	for (int i = 0; i < SPARK_COUNT; i++) {
		/* Deterministic spread - no RNG needed and it looks the same on
		 * every boot, which makes comparing the variants easier. */
		sparks[i].x  = ((i * 37) % (r)) - r / 2;
		sparks[i].y  = ((i * 53) % (r)) - r / 2;
		sparks[i].vx = ((i % 5) - 2) * 2 + 1;
		sparks[i].vy = ((i % 7) - 3) * 2 + 1;
	}

	sparksReady = true;
}

static void drawSparks(RenderContext *ctx, int cx, int cy, int r, int level) {
	if (!sparksReady)
		resetSparks(r);

	for (int i = 0; i < SPARK_COUNT; i++) {
		Spark *s = &sparks[i];

		s->x += s->vx;
		s->y += s->vy;

		int ax = s->x < 0 ? -s->x : s->x;
		int ay = s->y < 0 ? -s->y : s->y;

		/* Outside the rhombus? Flip whichever axis is pushing hardest. */
		if (ax + ay > r - 4) {
			if (ax > ay)
				s->vx = -s->vx;
			else
				s->vy = -s->vy;

			s->x += s->vx;
			s->y += s->vy;
		}

		uint32_t c = scaleCol(0x80c0ff, level);   /* warm white */

		drawRect(ctx, cx + s->x, cy + s->y, 2, 2, c, true);
	}
}

/* ---- variant 2: a 3D diamond (octahedron) --------------------------- */

#define OCTA_FACES 8

static const int octaFace[OCTA_FACES][3] = {
	{0, 2, 4}, {0, 4, 3}, {0, 3, 5}, {0, 5, 2},
	{1, 4, 2}, {1, 3, 4}, {1, 5, 3}, {1, 2, 5},
};

/*
 * Draw the diamond as a rotating octahedron.
 *
 * Six vertices on the axes, eight triangular faces, painter-sorted by
 * average depth. Projection is the same simple perspective the rest of this
 * project uses - divide by z - rather than the GTE, because this runs a
 * handful of vertices per frame and setting up the coprocessor's matrices
 * would cost more code than it saves.
 */
static void drawOctahedron(
	RenderContext *ctx, int cx, int cy, int r,
	int yaw, int pitch, int zOffset, int level
) {
	int vx[6], vy[6], vz[6];

	const int base[6][3] = {
		{ 0,  1,  0}, { 0, -1,  0},
		{ 1,  0,  0}, {-1,  0,  0},
		{ 0,  0,  1}, { 0,  0, -1},
	};

	int sy = isin(yaw),   cyw = icos(yaw);
	int sp = isin(pitch), cp  = icos(pitch);

	for (int i = 0; i < 6; i++) {
		int x = base[i][0] * r;
		int y = base[i][1] * r;
		int z = base[i][2] * r;

		/* Yaw about Y, then pitch about X. */
		int x1 = (x * cyw + z * sy) >> ISIN_SHIFT;
		int z1 = (z * cyw - x * sy) >> ISIN_SHIFT;
		int y1 = (y * cp - z1 * sp) >> ISIN_SHIFT;
		int z2 = (z1 * cp + y * sp) >> ISIN_SHIFT;

		int depth = z2 + zOffset;

		if (depth < 32)
			depth = 32;

		vx[i] = cx + (x1 * 220) / depth;
		vy[i] = cy + (y1 * 220) / depth;
		vz[i] = depth;
	}

	/* Painter's algorithm: draw the far faces first. */
	int order[OCTA_FACES];
	int key[OCTA_FACES];

	for (int f = 0; f < OCTA_FACES; f++) {
		order[f] = f;
		key[f] = (vz[octaFace[f][0]] + vz[octaFace[f][1]]
		        + vz[octaFace[f][2]]) / 3;
	}

	for (int a = 0; a < OCTA_FACES - 1; a++) {
		for (int b = a + 1; b < OCTA_FACES; b++) {
			if (key[order[b]] > key[order[a]]) {
				int tmp = order[a];
				order[a] = order[b];
				order[b] = tmp;
			}
		}
	}

	for (int i = 0; i < OCTA_FACES; i++) {
		int f = order[i];
		const int *fv = octaFace[f];

		/* Shade by face index so adjacent faces separate, then by the
		 * fade level. Alternating warm tones keep it reading as the same
		 * orange mark rather than a grey solid. */
		uint32_t base_c = (f & 1) ? 0x0517e0 : 0x0093df;
		uint32_t c = scaleCol(base_c, level);

		GPUDMAChain *chain = getCurrentChain(ctx);
		uint32_t *ptr = allocateGP0Packet(chain, 5);

		ptr[0] = c | gp0_shadedQuad(false, false, false);
		ptr[1] = gp0_xy(vx[fv[0]], vy[fv[0]]);
		ptr[2] = gp0_xy(vx[fv[1]], vy[fv[1]]);
		ptr[3] = gp0_xy(vx[fv[2]], vy[fv[2]]);
		ptr[4] = gp0_xy(vx[fv[2]], vy[fv[2]]);
	}
}

/* ---- variant 3: white Gouraud wave field ---------------------------- */

/*
 * The menu's wave theme, drawn in white instead of blue.
 *
 * Not a call into xmb_bg.c: that renderer's colours come from the theme
 * palette and there is no white entry, so recolouring it would mean adding a
 * palette nobody can select. Three sine bands with a vertical falloff is the
 * same shape of effect and stays local to the intro.
 */
static void drawWhiteWaves(RenderContext *ctx, int frame) {
	static const struct { int baseY, amp, freq, speed, height, shade; } layers[] = {
		{ 118, 14, 26, 6, 46, 232 },
		{ 132, 18, 34, 9, 38, 214 },
		{ 126, 11, 20, 5, 52, 244 },
	};

	for (unsigned l = 0; l < sizeof(layers) / sizeof(layers[0]); l++) {
		int prevX = 0, prevY = 0;

		for (int i = 0; i <= 12; i++) {
			int x = (i * 320) / 12;
			int a = (layers[l].freq * i + frame * layers[l].speed) * 4;
			int y = layers[l].baseY
			      + ((isin(a & (ISIN_PI * 2 - 1)) * layers[l].amp) >> ISIN_SHIFT);

			if (i > 0) {
				for (int s = 0; s < layers[l].height; s += 2) {
					int k = 256 - (s * 256) / layers[l].height;
					int g = 255 - ((255 - layers[l].shade) * k) / 256;

					uint32_t c = ((uint32_t) g << 16) | ((uint32_t) g << 8) | g;

					drawRect(ctx, prevX, prevY + s, x - prevX, 2, c, false);
				}
			}

			prevX = x;
			prevY = y;
		}
	}
}

void setPS1IntroVariant(int variant) {
	if (variant >= 0 && variant < PS1_INTRO_COUNT)
		introVariant = variant;
}

void initPS1Boot(RenderContext *ctx) {
	(void) ctx;

#if PS1_BOOT_TEXTURES

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
#endif
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
		if (introVariant == PS1_INTRO_SPIN3D) {
			/*
			 * Variant 2: the mark as a real 3D octahedron that flies in
			 * from the distance, turning about once, and settles facing
			 * the camera at the size the flat version occupies.
			 *
			 * The whole move happens during the diamond's own fade-in
			 * window plus the triangle window, so the rest of the
			 * timeline is untouched.
			 */
			int k = ramp(frame, T_DIAMOND_IN, T_TRI_END);

			/* Ease out: fast approach, gentle settle. */
			int ease = 256 - (((256 - k) * (256 - k)) >> 8);

			int zOff = 900 - ((900 - 260) * ease) / 256;
			int yaw  = ((256 - ease) * (ISIN_PI * 2)) / 256;   /* ~one turn */
			int pitch = ((256 - ease) * 180) / 256;

			drawOctahedron(ctx, cx, cy, r, yaw & (ISIN_PI * 2 - 1),
				pitch, zOff, 256);
			return;
		}

		if (introVariant == PS1_INTRO_GLASS ||
		    introVariant == PS1_INTRO_GLASS_WAVES) {
			/*
			 * Variants 1 and 3: the same mark as glass.
			 *
			 * Three blended rings outside the shape give an outward glow -
			 * the same accumulate-by-blending trick the memory card tiles
			 * use, since the GPU has no real additive mode here. The body
			 * is then drawn blended so the background shows through, and
			 * the sparks bounce around inside it.
			 */
			for (int g = 3; g >= 1; g--) {
				int gr = r + g * 5;
				uint32_t gc = scaleCol(0x0060c0, 256 / (g + 1));

				glassQuad(ctx,
					cx - gr, cy, cx, cy - gr,
					cx, cy + gr, cx + gr, cy, gc);
			}

			/* Glass body: two blended passes so it reads as tinted rather
			 * than a faint wash, but still see-through. */
			for (int pass = 0; pass < 2; pass++)
				glassQuad(ctx,
					cx - r, cy, cx, cy - r,
					cx, cy + r, cx + r, cy,
					scaleCol(0x0060b0, level));

			/* Bright top-left facet, as on the tiles. */
			glassQuad(ctx,
				cx - r, cy, cx, cy - r,
				cx, cy, cx, cy,
				scaleCol(0x00a0f0, level));

			drawSparks(ctx, cx, cy, r, level);
			return;
		}

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

		/* Sizes: 25%x50% -> 10%x20% of the square. */
		int w = ((25 + ((10 - 25) * k) / 256) * SQ) / 100;
		int h = ((50 + ((20 - 50) * k) / 256) * SQ) / 100;

		/* Top-left corners, in percent * 100 to keep the interpolation
		 * honest with integer maths. */
		int t1x = ((2500 + ((4100 - 2500) * k) / 256) * SQ) / 10000;
		int t1y = ((2500 + ((3100 - 2500) * k) / 256) * SQ) / 10000;
		int t2x = ((5000 + ((4900 - 5000) * k) / 256) * SQ) / 10000;
		int t2y = ((2500 + ((4900 - 2500) * k) / 256) * SQ) / 10000;

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
		spriteFadeOnLight(ctx, &sonyTex,
			SQ_X + (SQ - SONY_DW) / 2, PCT_Y(5), SONY_DW, SONY_DH, textLevel);

		spriteFadeOnLight(ctx, &sceTextTex,
			SQ_X + (SQ - SCET_DW) / 2, PCT_Y(94) - SCET_DH,
			SCET_DW, SCET_DH, textLevel);
#endif

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
#if PS1_BOOT_TEXTURES
		uint32_t g = (uint32_t) clampi((logoLevel * 128) / 256, 0, 128);
		uint32_t tint = (g << 16) | (g << 8) | g;

		sprite(ctx, &psLogoTex,
			SQ_X + (SQ - PSLG_DW) / 2, PCT_Y(16), PSLG_DW, PSLG_DH, tint, false);
#endif
	}

	if (textLevel > 0) {
#if PS1_BOOT_TEXTURES
		uint32_t g = (uint32_t) clampi((textLevel * 128) / 256, 0, 128);
		uint32_t tint = (g << 16) | (g << 8) | g;

		sprite(ctx, &psTextTex,
			SQ_X + (SQ - PSTX_DW) / 2, PCT_Y(47), PSTX_DW, PSTX_DH, tint, false);
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
				PCT_Y(61) + i * 12, col, lines[i]);
		}
	}
}

/*
 * Variant picker.
 *
 * Deliberately plain: a dark screen, four lines, D-pad and X. It runs before
 * any of the boot artwork is uploaded, so it cannot depend on it.
 */
int chooseIntroVariant(RenderContext *ctx) {
	static const char *const names[PS1_INTRO_COUNT] = {
		"1  Classic      flat gradient, as original",
		"2  Glass        see-through, sparks, glow",
		"3  3D Diamond   spins in and settles",
		"4  Glass+Waves  glass over white waves",
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
				sel = (sel + PS1_INTRO_COUNT - 1) % PS1_INTRO_COUNT;
			if (pressed & PAD_BTN_DOWN)
				sel = (sel + 1) % PS1_INTRO_COUNT;
			if (pressed & (PAD_BTN_CROSS | PAD_BTN_START))
				break;
		}

		beginFrame(ctx);
		drawRect(ctx, 0, 0, ctx->screenWidth, ctx->screenHeight,
			0x201008, false);

		printString(ctx, 16, 24, 0xffffff, "INTRO STYLE TEST");
		printString(ctx, 16, 40, 0x808080,
			"D-PAD choose      " CH_PS1_CROSS_BUTTON " start");

		for (int i = 0; i < PS1_INTRO_COUNT; i++) {
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
			white ? 0xffffff : 0x000000, false);

		// Variant 4 replaces the flat white field with a wave field, still
		// in white so the black artwork on top stays readable.
		if (white && introVariant == PS1_INTRO_GLASS_WAVES)
			drawWhiteWaves(ctx, frame);

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
