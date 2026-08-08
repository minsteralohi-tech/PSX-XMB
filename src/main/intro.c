/*
 * PSX-iTests - boot intro
 *
 * A five-second startup animation in the style of the PSP/PS3 boot sequence:
 * the wave field rises out of black, a title fades in over it, the waves then
 * accelerate and stretch out, and everything fades away into the menu.
 *
 * WHY THIS IS NOT A VIDEO, AND NOT A 3D MODEL
 * -------------------------------------------
 * The obvious approaches both lose here. MDEC video (.STR) means the intro
 * only exists when booting from a disc, and this dashboard also runs from a
 * flash cart and over serial. A Blender-authored 3D model means new geometry,
 * a new exporter and a new renderer - a lot of moving parts for something the
 * project can already draw.
 *
 * Because the dashboard already HAS this animation: xmb_bg.c's Gouraud wave
 * field with sparkles, which is hardware-proven and running as a live theme.
 * So the intro is not new artwork at all - it drives the existing renderer
 * with a scripted clock and an overlay. That means it costs almost no RAM, it
 * cannot look out of place next to the menu it hands over to, and it works
 * from every boot medium.
 *
 * TIMING
 * ------
 * 300 frames at 60Hz. On a PAL console the field rate is 50Hz, so the intro
 * runs about a second longer there; that is a deliberate trade against
 * building a region-aware clock for a one-shot animation nobody is timing
 * with a stopwatch.
 *
 *     0.0 - 1.0s   waves rise out of black
 *     1.0 - 2.3s   title fades in, waves drift gently
 *     2.3 - 3.8s   waves accelerate and stretch
 *     3.8 - 5.0s   title and waves fade to black, menu takes over
 */

#include <stdbool.h>
#include <stdint.h>
#include "common/sio0.h"
#include "main/defs.h"
#include "main/font.h"
#include "main/intro.h"
#include "main/renderer.h"
#include "main/xmb_bg.h"
#include "ps1/registers.h"

/*
 * The title text.
 *
 * Isolated here on purpose so it is a one-line change. It is the string the
 * original hardware showed at boot, which is the point of the homage, but if
 * this build is ever shared around it is worth swapping for something of your
 * own - trademarked wordmarks and redistribution do not mix well.
 */
#define INTRO_TITLE "Sony Computer Entertainment"

#define INTRO_FRAMES     300

#define PHASE_RISE_END    60   /* 1.0s */
#define PHASE_TITLE_END  138   /* 2.3s */
#define PHASE_RUSH_END   228   /* 3.8s */

/*
 * Fade steps.
 *
 * The GPU's semi-transparent blend is a fixed 50% mix, so a black overlay
 * drawn once leaves 50% of the picture, twice 25%, three times 12.5% and so
 * on - each pass halves what remains. That gives five usable brightness
 * levels rather than a smooth ramp, which is the honest limit of this
 * hardware without a texture-based dither. Stepping through them over ~20
 * frames apiece reads as a fade rather than as four visible jumps.
 */
#define FADE_MAX_PASSES 5

static void drawFadeToBlack(RenderContext *ctx, int passes) {
	for (int i = 0; i < passes; i++)
		drawRect(ctx, 0, 0, ctx->screenWidth, ctx->screenHeight, 0x000000, true);
}

/* Scale a 0xBBGGRR colour by n/256, for fading the title text. */
static uint32_t scaleColour(uint32_t colour, int n) {
	uint32_t r = ((colour        & 0xff) * n) >> 8;
	uint32_t g = (((colour >> 8)  & 0xff) * n) >> 8;
	uint32_t b = (((colour >> 16) & 0xff) * n) >> 8;

	return (b << 16) | (g << 8) | r;
}

void runBootIntro(RenderContext *ctx) {
	// Force the PSP-style wave theme for the duration, then put the user's
	// own choice back. The intro should look the same on every boot rather
	// than inheriting whatever theme was last selected.
	uint8_t savedTheme = xmbThemeIndex;

	xmbThemeIndex = XMB_THEME_GOURAUD_SPARKLE;
	int skipHoldFrames =
		((GPU_GP1 & GP1_STAT_FB_MODE_BITMASK) == GP1_STAT_FB_MODE_PAL)
			? 150 : 180;
	int startHoldFrames = 0;

	for (int frame = 0; frame < INTRO_FRAMES; frame++) {
		uint16_t buttons = pollController(0) | pollController(1);
		if (buttons & PAD_BTN_START) {
			if (++startHoldFrames >= skipHoldFrames)
				break;
		} else {
			startHoldFrames = 0;
		}

		beginFrame(ctx);

		/*
		 * Wave speed. drawXMBBackground() advances the clock one frame on
		 * its own; this adds more during the rush so the waves visibly
		 * stretch and pick up pace, then eases back off.
		 */
		if (frame > PHASE_TITLE_END && frame <= PHASE_RUSH_END) {
			int into = frame - PHASE_TITLE_END;
			int span = PHASE_RUSH_END - PHASE_TITLE_END;

			// Ramp up to +7 extra frames per frame, i.e. 8x speed at peak.
			xmbAdvanceFrames((uint32_t) (into * 7 / span));
		} else if (frame > PHASE_RUSH_END) {
			// Coast down through the fade rather than stopping dead.
			int into = frame - PHASE_RUSH_END;
			int span = INTRO_FRAMES - PHASE_RUSH_END;
			int left = span - into;

			xmbAdvanceFrames((uint32_t) (left * 7 / span));
		}

		drawXMBBackground(ctx);

		/* --- title ---------------------------------------------------- */
		int titleLevel = 0;   /* 0..256 */

		if (frame >= PHASE_RISE_END && frame <= PHASE_TITLE_END) {
			// Fade in across the title phase.
			titleLevel = (frame - PHASE_RISE_END) * 256
			           / (PHASE_TITLE_END - PHASE_RISE_END);
		} else if (frame > PHASE_TITLE_END && frame <= PHASE_RUSH_END) {
			titleLevel = 256;
		} else if (frame > PHASE_RUSH_END) {
			// Fade out over the last stretch.
			int into = frame - PHASE_RUSH_END;
			int span = INTRO_FRAMES - PHASE_RUSH_END;

			titleLevel = 256 - (into * 256 / span);
		}

		if (titleLevel > 0) {
			if (titleLevel > 256)
				titleLevel = 256;

			int textW = getStringWidth(INTRO_TITLE);

			printString(
				ctx,
				(ctx->screenWidth - textW) / 2,
				ctx->screenHeight / 2 - 4,
				scaleColour(0xffffff, titleLevel),
				INTRO_TITLE
			);
		}

		/* --- black overlay -------------------------------------------- */
		int passes = 0;

		if (frame < PHASE_RISE_END) {
			// Rising out of black: fewer passes as the waves come up.
			passes = FADE_MAX_PASSES
			       - (frame * FADE_MAX_PASSES / PHASE_RISE_END);
		} else if (frame > PHASE_RUSH_END) {
			// Sinking back into it, so the menu appears from black.
			int into = frame - PHASE_RUSH_END;
			int span = INTRO_FRAMES - PHASE_RUSH_END;

			passes = into * FADE_MAX_PASSES / span;
		}

		if (passes > 0)
			drawFadeToBlack(ctx, passes);

		endFrame(ctx);
	}

	xmbThemeIndex = savedTheme;

	// Do not hand a held button straight to the menu - otherwise skipping
	// the intro also picks whatever the cursor happens to be sitting on.
	for (int guard = 0; guard < skipHoldFrames; guard++) {
		if (!(pollController(0) | pollController(1)))
			break;
	}
}
