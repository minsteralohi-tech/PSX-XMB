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

/*
 * The standalone loader does not need a cold-boot RAM state - it validates
 * and stages everything itself, and its own linker script keeps it clear of
 * the region it receives into. Erasing the dashboard first is therefore not
 * required, and skipping it keeps the handoff as short as possible.
 *
 * Set this to 1 for a target that genuinely assumes clean RAM. Stage 1
 * excludes the payload, the arena and the BIOS's first 64 KB from the fill,
 * so it is safe either way.
 */
#define SIO_LOADER_ERASE_RAM 0

/* UniROM manages RAM and the kernel itself - see runUniROMLauncher(). */
#define UNIROM_ERASE_RAM 1


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
 * Confirmation screen. Triangle toggles the RAM erase and re-plans, which
 * changes the handoff between "direct" and "stage 1" - so both paths can be
 * tried on real hardware without a rebuild. Returns non-zero if confirmed,
 * with *plan holding whatever the user settled on.
 */
static int confirmHandoff(
	RenderContext  *ctx,
	const char     *title,
	const char     *blurb1,
	const char     *blurb2,
	const uint8_t  *exe,
	int            *eraseRam,
	AppLaunchPlan  *plan
) {
	char line[64];

	waitForRelease();

	for (;;) {
		uint16_t buttons = pollController(0) | pollController(1);

		if (buttons & PAD_BTN_CIRCLE) {
			playCancelSound();
			waitForRelease();
			return 0;
		}

		if (buttons & PAD_BTN_TRIANGLE) {
			/* Flip the erase and re-plan. If the new setting cannot be
			 * planned, keep the old one rather than showing a broken
			 * screen. */
			AppLaunchPlan retry;

			if (planEmbeddedApp(exe, !*eraseRam, &retry) == APP_PLAN_OK) {
				*eraseRam = !*eraseRam;
				*plan = retry;
			}

			waitForRelease();
			continue;
		}

		if (buttons & PAD_BTN_CROSS)
			break;

		beginFrame(ctx);
		drawXMBBackground(ctx);

		printString(ctx, 16, 30, 0xffffff, title);
		printString(ctx, 16, 58, 0xffffff,
			"This will leave the PSX-iTests dashboard.");
		printString(ctx, 16, 74, 0xffffff, blurb1);
		printString(ctx, 16, 90, 0xffffff, blurb2);

		printString(ctx, 16, 118, 0x808080, "Launch plan");

		snprintf(line, sizeof(line), "Target   %08lX - %08lX  (%lu bytes)",
			(unsigned long) plan->dest,
			(unsigned long) plan->destEnd,
			(unsigned long) plan->bodySize);
		printString(ctx, 24, 134, 0xffffff, line);

		snprintf(line, sizeof(line), "Arena    %08lX      Entry %08lX",
			(unsigned long) plan->arena,
			(unsigned long) plan->entry);
		printString(ctx, 24, 148, 0xffffff, line);

		snprintf(line, sizeof(line), "Dash end %08lX      Live  %08lX",
			(unsigned long) plan->imageEnd,
			(unsigned long) plan->liveEnd);
		printString(ctx, 24, 162, 0xffffff, line);

		/* Which handoff this will use. "Direct" is the same copy-and-jump
		 * that has always been used here; "Stage 1" is the trampoline. */
		snprintf(line, sizeof(line), "Path     %s",
			plan->useStage1 ? "Stage 1 trampoline" : "Direct copy + jump");
		printString(ctx, 24, 176, 0x60ff60, line);

		snprintf(line, sizeof(line), "Erase RAM  %s",
			*eraseRam ? "YES - wipe the dashboard first"
			          : "no  - leave RAM as it is");
		printString(ctx, 24, 190, 0x60ff60, line);

		printString(ctx, 16, ctx->screenHeight - 40, 0x808080,
			"Reset or power-cycle to return to the dashboard.");

		printString(ctx, 16, ctx->screenHeight - 26, 0x606060,
			CH_PS1_CROSS_BUTTON " Enter    "
			CH_PS1_CIRCLE_BUTTON " Back    "
			CH_PS1_TRIANGLE_BUTTON " Erase RAM");

		endFrame(ctx);
	}

	waitForRelease();
	return 1;
}

/*
 * Shared entry point for both menu items: plan, show, confirm, launch.
 */
static void runLaunchScreen(
	RenderContext *ctx,
	const char    *title,
	const char    *blurb1,
	const char    *blurb2,
	const uint8_t *exe,
	int            eraseRam
) {
	AppLaunchPlan plan;
	AppPlanResult result = planEmbeddedApp(exe, eraseRam, &plan);

	if (result != APP_PLAN_OK) {
		/* Fall back to the other erase setting before giving up - one of
		 * the two may be unplannable on a given build. */
		result = planEmbeddedApp(exe, !eraseRam, &plan);

		if (result == APP_PLAN_OK)
			eraseRam = !eraseRam;
	}

	if (result != APP_PLAN_OK) {
		showPlanError(ctx, result);
		return;
	}

	if (!confirmHandoff(ctx, title, blurb1, blurb2, exe, &eraseRam, &plan))
		return;

	/* Never returns. */
	runAppLaunch(&plan);
}

void runSIOLoader(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
) {
	(void) state;
	(void) item;

	runLaunchScreen(
		ctx,
		"SIO LOADER",
		"The standalone serial loader will start and",
		"wait for a PS-EXE on SIO1 at 115200 8N2.",
		sioLoaderExe,
		SIO_LOADER_ERASE_RAM
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
	 * UniROM installs its own kernel exception handler and TTY redirect and
	 * manages RAM itself, so it is the most plausible candidate for needing
	 * a cold-boot-like RAM state. That is on by default here; Triangle on
	 * the confirmation screen flips it, which also switches the handoff
	 * between the trampoline and the direct copy - so all four combinations
	 * can be tried on hardware without a rebuild.
	 *
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
		UNIROM_ERASE_RAM
	);
}
