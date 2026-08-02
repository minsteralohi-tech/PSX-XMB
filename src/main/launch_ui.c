/*
 * PSX-iTests - launch confirmation screens
 *
 * The dashboard no longer contains a serial receiver. This entry hands the
 * console over to the standalone SIO loader (assets/sioloader.exe, embedded
 * as sioLoaderExe) through the generic two-stage launcher in app_launch.c.
 *
 * Everything the launch depends on is worked out first and shown on screen
 * before anything is committed: where the loader will live, where stage 1
 * will run, and where this dashboard currently ends. A failed launch used to
 * mean staring at a black screen and guessing; now the addresses that would
 * have been involved are visible beforehand, and an impossible launch is
 * refused with a reason instead of attempted.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "common/sio0.h"
#include "main/app_launch.h"
#include "main/defs.h"
#include "main/font.h"
#include "main/launch_ui.h"
#include "main/xmb_menu.h"
#include "main/sound.h"
#include "main/xmb_bg.h"

/* The standalone loader, embedded via addBinaryFile() in CMakeLists.txt. */
extern const uint8_t sioLoaderExe[];
extern const uint8_t uniromExe[];
extern const uint8_t suite240pExe[];
extern const uint8_t sony41BiosExe[];

/*
 * Per-app launch settings.
 *
 * Every one of these was established by testing on real hardware, and they are
 * fixed rather than offered as options - the user should press X and have it
 * work. See docs/TWO-STAGE-LAUNCHER.md for the full history.
 *
 *   SIO loader   0x801B0000, bare-metal, one BIOS call in its whole life.
 *                Works under every combination tested. Uses the plain direct
 *                copy and jump: it disturbs the least state, and it is the
 *                path with the longest track record here.
 *
 *   UniROM       0x801D0000. Also works under every combination now that the
 *                dashboard no longer installs a TTY device (its own start-up
 *                calls RemoveDevice("tty"), which used to invoke our driver's
 *                deinit handler and block forever on SIO1 TX-ready). Same
 *                direct path, and no RAM erase: it loads above the dashboard,
 *                so there is nothing to clear out of its way.
 *
 *   240p suite   0x80010000, straight over the dashboard, so the stage 1
 *                trampoline is not optional. It ALSO requires the RAM erase:
 *                its PS-EXE header declares no BSS at all (bssAddr and bssSize
 *                are both zero), so nothing - not the BIOS, not us - zeroes
 *                its uninitialised data. It gets away with that from a cold
 *                boot or over serial because the RAM it lands in happens to be
 *                clear; after this dashboard has been running, it is full of
 *                our leftovers. Erasing RAM first is what makes it work, and
 *                it is the one target here that needs it.
 */
typedef struct {
	int eraseRam;
	int biosExec;
} LaunchConfig;

static const LaunchConfig SIO_LOADER_CONFIG = { 0, 0 };
static const LaunchConfig UNIROM_CONFIG     = { 0, 0 };
static const LaunchConfig SUITE240P_CONFIG  = { 1, 0 };

/*
 * The Sony 4.1A BIOS shell is UNPROVEN on hardware, so unlike the four above
 * its entry does not fix the hand-off - the confirmation screen exposes the
 * options so every combination can be tried without a rebuild. Once a working
 * combination is known, put it here and switch it to the plain dialog.
 *
 * It loads at 0x8002F000, straight over this dashboard, so planEmbeddedApp()
 * will always route it through the stage 1 trampoline; the Exec() option is
 * therefore unavailable and planUseBiosExec() will refuse it. Starting point
 * is erase-RAM on, matching the 240p suite, which is the other target that
 * lands on top of the dashboard.
 */
static const LaunchConfig SONY41_CONFIG     = { 1, 0 };

static void waitForRelease(void) {
	while (pollController(0) | pollController(1))
		;
}

static void showPlanError(
	RenderContext *ctx,
	AppPlanResult  result
) {
	char line[64];

	waitForRelease();

	for (;;) {
		uint16_t buttons = pollController(0) | pollController(1);

		if (buttons & (PAD_BTN_CIRCLE | PAD_BTN_CROSS))
			break;

		beginFrame(ctx);
		drawXMBBackground(ctx);

		printString(ctx, 16, 30, 0xffffff, "SIO LOADER");
		printString(ctx, 16, 58, 0xff6060, "Cannot launch the standalone loader.");

		snprintf(line, sizeof(line), "Reason: %s", appPlanResultText(result));
		printString(ctx, 16, 82, 0xffffff, line);

		printString(ctx, 16, 110, 0x808080,
			"The embedded loader image or this build's RAM");
		printString(ctx, 16, 126, 0x808080,
			"layout has changed. Nothing was launched.");

		printString(ctx, 16, ctx->screenHeight - 26, 0x606060,
			CH_PS1_CIRCLE_BUTTON " Back");

		endFrame(ctx);
	}

	waitForRelease();
}

/*
 * Confirmation screen: what the launch is, and X to do it.
 *
 * This used to expose the erase and Exec() choices as Triangle/Square toggles
 * and print the full address plan. That was scaffolding for working out which
 * combination each app needed; now that each one is known, it is just noise.
 */
/*
 * A readable panel: one flat tint, bevelled edge, no sheen.
 *
 * The same treatment the memory card manager's Options popup uses. Not
 * drawGlassPanel(): its specular band puts two different tones behind the
 * same line of text, which is fine on a small tile and bad behind a
 * paragraph. Drawing the body twice settles it to roughly 75% opacity -
 * blending is a fixed 50% mix on this hardware, so each pass halves what
 * still shows through.
 */
static uint32_t shade(uint32_t colour, int numerator, int denominator) {
	uint32_t r = ((colour        & 0xff) * numerator) / denominator;
	uint32_t g = (((colour >> 8)  & 0xff) * numerator) / denominator;
	uint32_t b = (((colour >> 16) & 0xff) * numerator) / denominator;

	if (r > 0xff) r = 0xff;
	if (g > 0xff) g = 0xff;
	if (b > 0xff) b = 0xff;

	return (b << 16) | (g << 8) | r;
}

static void drawDialogCard(
	RenderContext *ctx, int x, int y, int w, int h, uint32_t tint
) {
	uint32_t body = shade(tint, 3, 4);

	drawRect(ctx, x,     y + 1,     w,     h - 2, body, true);
	drawRect(ctx, x,     y + 1,     w,     h - 2, body, true);
	drawRect(ctx, x + 1, y,         w - 2, 1,     body, true);
	drawRect(ctx, x + 1, y + h - 1, w - 2, 1,     body, true);

	uint32_t lit   = shade(tint, 5, 2);
	uint32_t shadeC = shade(tint, 1, 3);

	drawRect(ctx, x + 1,     y,         w - 2, 1,     lit,    true);
	drawRect(ctx, x,         y + 1,     1,     h - 2, lit,    true);
	drawRect(ctx, x + 1,     y + h - 1, w - 2, 1,     shadeC, true);
	drawRect(ctx, x + w - 1, y + 1,     1,     h - 2, shadeC, true);
}

/*
 * Exit confirmation.
 *
 * This used to be a full screen of text. A dialog over the live dashboard
 * reads better and, more usefully, makes it obvious that nothing has happened
 * yet - the menu is still there behind it.
 */
static int confirmHandoff(
	RenderContext *ctx,
	UIState       *state,
	const char    *title
) {
	const char *line1 = "You are exiting the PSX Dashboard.";
	const char *line2 = "Reset the console to return to the menu";
	const char *keys  = CH_PS1_CROSS_BUTTON ": OK   "
	                    CH_PS1_CIRCLE_BUTTON ": Back";

	// Hug the widest line rather than guessing a width. getStringWidth()
	// handles the button glyphs correctly now that it stopped treating them
	// as negative char values.
	int textW = getStringWidth(line1);
	int w2    = getStringWidth(line2);
	int w3    = getStringWidth(keys);

	if (w2 > textW) textW = w2;
	if (w3 > textW) textW = w3;

	int boxW = textW + 20;
	if (boxW > 300)
		boxW = 300;

	int boxX = (320 - boxW) / 2;
	int boxH = 92;
	int boxY = (240 - boxH) / 2;

	uint32_t accent, glow;

	waitForRelease();

	for (;;) {
		uint16_t buttons = pollController(0) | pollController(1);

		if (buttons & PAD_BTN_CIRCLE) {
			playCancelSound();
			waitForRelease();
			return 0;
		}

		if (buttons & PAD_BTN_CROSS) {
			playConfirmSound();
			break;
		}

		beginFrame(ctx);

		// The menu itself, so the dialog reads as an overlay on top of a
		// live screen rather than as a separate page - Circle visibly
		// returns to exactly where the user was.
		renderXMB(ctx, state);

		// Follow the wallpaper, same as the memory card tiles.
		xmbGetAccentColor(&accent, &glow);

		drawDialogCard(ctx, boxX, boxY, boxW, boxH, accent);

		printString(ctx, boxX + 10, boxY + 10, 0xffffff, title);
		printString(ctx, boxX + 10, boxY + 34, 0xffffff, line1);
		printString(ctx, boxX + 10, boxY + 50, 0xffffff, line2);
		// Plain white, not the theme accent: OK/Back are the same everywhere
		// in the dashboard and should not shift colour with the wallpaper.
		printString(ctx, boxX + 10, boxY + 72, 0xffffff, keys);

		endFrame(ctx);
	}

	waitForRelease();
	return 1;
}

/*
 * Test-mode confirmation, for a target whose working hand-off is not known
 * yet. Same crystal dialog, plus live toggles and the plan it would use, so a
 * failing combination can be identified on hardware without a rebuild.
 */
static int confirmHandoffTest(
	RenderContext *ctx,
	UIState       *state,
	const char    *title,
	const uint8_t *exe,
	int           *eraseRam,
	int           *biosExec,
	AppLaunchPlan *plan
) {
	char line[52];
	uint32_t accent, glow;

	int boxW = 296;
	int boxX = (320 - boxW) / 2;
	int boxH = 150;
	int boxY = (240 - boxH) / 2;

	waitForRelease();

	for (;;) {
		uint16_t buttons = pollController(0) | pollController(1);

		if (buttons & PAD_BTN_CIRCLE) {
			playCancelSound();
			waitForRelease();
			return 0;
		}

		if (buttons & PAD_BTN_TRIANGLE) {
			AppLaunchPlan retry;

			if (planEmbeddedApp(exe, !*eraseRam, &retry) == APP_PLAN_OK) {
				*eraseRam = !*eraseRam;

				if (!planUseBiosExec(&retry, *biosExec))
					*biosExec = 0;

				*plan = retry;
				playScrollSound();
			}

			waitForRelease();
			continue;
		}

		if (buttons & PAD_BTN_SQUARE) {
			if (planUseBiosExec(plan, !*biosExec)) {
				*biosExec = !*biosExec;
				playScrollSound();
			}

			waitForRelease();
			continue;
		}

		if (buttons & PAD_BTN_CROSS) {
			playConfirmSound();
			break;
		}

		beginFrame(ctx);
		renderXMB(ctx, state);

		xmbGetAccentColor(&accent, &glow);
		drawDialogCard(ctx, boxX, boxY, boxW, boxH, accent);

		printString(ctx, boxX + 10, boxY + 8, 0xffffff, title);
		printString(ctx, boxX + 10, boxY + 26, 0xffffff,
			"You are exiting the PSX Dashboard.");
		printString(ctx, boxX + 10, boxY + 40, 0xffffff,
			"Reset the console to return to the menu");

		snprintf(line, sizeof(line), "Target  %08lX - %08lX",
			(unsigned long) plan->dest, (unsigned long) plan->destEnd);
		printString(ctx, boxX + 10, boxY + 62, 0x808080, line);

		snprintf(line, sizeof(line), "Source  %08lX   Arena %08lX",
			(unsigned long) plan->src, (unsigned long) plan->arena);
		printString(ctx, boxX + 10, boxY + 74, 0x808080, line);

		snprintf(line, sizeof(line), "Path    %s",
			plan->useStage1   ? "Stage 1 trampoline" :
			plan->useBiosExec ? "BIOS Exec()"
			                  : "Direct copy + jump");
		printString(ctx, boxX + 10, boxY + 88, 0x1256e3, line);

		snprintf(line, sizeof(line), "Erase RAM  %s      Exec()  %s",
			*eraseRam ? "YES" : "no ", *biosExec ? "YES" : "no ");
		printString(ctx, boxX + 10, boxY + 102, 0x1256e3, line);

		printString(ctx, boxX + 10, boxY + 124, 0xffffff,
			CH_PS1_CROSS_BUTTON ": OK  "
			CH_PS1_CIRCLE_BUTTON ": Back  "
			CH_PS1_TRIANGLE_BUTTON ": Erase  "
			CH_PS1_SQUARE_BUTTON ": Exec");

		endFrame(ctx);
	}

	waitForRelease();
	return 1;
}

/*
 * Shared entry point for every launcher menu item: plan, confirm, launch.
 */
static void runLaunchScreen(
	RenderContext      *ctx,
	UIState            *state,
	const char         *title,
	const uint8_t      *exe,
	const LaunchConfig *config
) {
	AppLaunchPlan plan;
	AppPlanResult result = planEmbeddedApp(exe, config->eraseRam, &plan);

	if (result != APP_PLAN_OK) {
		showPlanError(ctx, result);
		return;
	}

	/* planUseBiosExec() refuses combinations that cannot work; none of the
	 * settings above ask for one, so a refusal here means the layout has
	 * shifted and the direct path is the safe answer. */
	if (config->biosExec)
		(void) planUseBiosExec(&plan, 1);

	if (!confirmHandoff(ctx, state, title))
		return;

	/* Never returns. */
	runAppLaunch(&plan);
}

void runSIOLoader(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
) {
	(void) item;

	runLaunchScreen(
		ctx,
		state,
		"SIO LOADER",
		sioLoaderExe,
		&SIO_LOADER_CONFIG
	);
}

void run240pSuite(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
) {
	(void) item;

	runLaunchScreen(
		ctx,
		state,
		"240P TEST SUITE",
		suite240pExe,
		&SUITE240P_CONFIG
	);
}

void runSony41Bios(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
) {
	(void) item;

	int eraseRam = SONY41_CONFIG.eraseRam;
	int biosExec = SONY41_CONFIG.biosExec;

	AppLaunchPlan plan;
	AppPlanResult result = planEmbeddedApp(sony41BiosExe, eraseRam, &plan);

	if (result != APP_PLAN_OK) {
		result = planEmbeddedApp(sony41BiosExe, !eraseRam, &plan);

		if (result == APP_PLAN_OK)
			eraseRam = !eraseRam;
	}

	if (result != APP_PLAN_OK) {
		showPlanError(ctx, result);
		return;
	}

	if (!planUseBiosExec(&plan, biosExec))
		biosExec = 0;

	if (!confirmHandoffTest(ctx, state, "SONY 4.1 BIOS",
	                        sony41BiosExe, &eraseRam, &biosExec, &plan))
		return;

	/* Never returns. */
	runAppLaunch(&plan);
}

void runUniROMLauncher(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
) {
	(void) item;

	runLaunchScreen(
		ctx,
		state,
		"UNIROM 8.0",
		uniromExe,
		&UNIROM_CONFIG
	);
}
