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
#include "main/sound.h"
#include "main/xmb_bg.h"

/* The standalone loader, embedded via addBinaryFile() in CMakeLists.txt. */
extern const uint8_t sioLoaderExe[];
extern const uint8_t uniromExe[];
extern const uint8_t suite240pExe[];

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
static int confirmHandoff(
	RenderContext *ctx,
	const char    *title,
	const char    *blurb1,
	const char    *blurb2
) {
	waitForRelease();

	for (;;) {
		uint16_t buttons = pollController(0) | pollController(1);

		if (buttons & PAD_BTN_CIRCLE) {
			playCancelSound();
			waitForRelease();
			return 0;
		}

		if (buttons & PAD_BTN_CROSS)
			break;

		beginFrame(ctx);
		drawXMBBackground(ctx);

		printString(ctx, 16, 30, 0xffffff, title);
		printString(ctx, 16, 66, 0xffffff,
			"This will leave the PSX-iTests dashboard.");
		printString(ctx, 16, 82, 0xffffff, blurb1);
		printString(ctx, 16, 98, 0xffffff, blurb2);

		printString(ctx, 16, 130, 0x808080,
			"Reset or power-cycle to come back.");

		printString(ctx, 16, ctx->screenHeight - 26, 0x606060,
			CH_PS1_CROSS_BUTTON " Enter    "
			CH_PS1_CIRCLE_BUTTON " Back");

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
	const char         *title,
	const char         *blurb1,
	const char         *blurb2,
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

	if (!confirmHandoff(ctx, title, blurb1, blurb2))
		return;

	/* Never returns. */
	runAppLaunch(&plan);
}

void run240pSuite(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
) {
	(void) state;
	(void) item;

	runLaunchScreen(
		ctx,
		"240P TEST SUITE",
		"Artemio's 240p Test Suite will be copied",
		"over this dashboard and started.",
		suite240pExe,
		&SUITE240P_CONFIG
	);
}

void runUniROMLauncher(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
) {
	(void) state;
	(void) item;

	/*
	 * If a real UniROM cartridge is installed, use the "UniROM (cart
	 * installed)" entry instead: this one copies a second, independent
	 * UniROM image into RAM while the cart's firmware is still resident.
	 */
	runLaunchScreen(
		ctx,
		"UNIROM 8.0",
		"UniROM will be copied to RAM and started.",
		"For a real UniROM cart, use the cart entry.",
		uniromExe,
		&UNIROM_CONFIG
	);
}
