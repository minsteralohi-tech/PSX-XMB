/*
 * PSX-iTests - Settings -> SIO Loader
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
#include "main/sio_launch.h"
#include "main/sound.h"
#include "main/xmb_bg.h"

/* The standalone loader, embedded via addBinaryFile() in CMakeLists.txt. */
extern const uint8_t sioLoaderExe[];

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

/* Returns non-zero if the user confirmed the handoff. */
static int confirmSIOLoaderHandoff(
	RenderContext        *ctx,
	const AppLaunchPlan  *plan
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

		if (buttons & PAD_BTN_CROSS)
			break;

		beginFrame(ctx);
		drawXMBBackground(ctx);

		printString(ctx, 16, 30, 0xffffff, "SIO LOADER");
		printString(ctx, 16, 58, 0xffffff,
			"This will leave the PSX-iTests dashboard.");
		printString(ctx, 16, 74, 0xffffff,
			"The standalone serial loader will start and");
		printString(ctx, 16, 90, 0xffffff,
			"wait for a PS-EXE on SIO1 at 115200 8N2.");

		printString(ctx, 16, 118, 0x808080, "Launch plan");

		snprintf(line, sizeof(line), "Loader   %08lX - %08lX  (%lu bytes)",
			(unsigned long) plan->dest,
			(unsigned long) plan->destEnd,
			(unsigned long) plan->bodySize);
		printString(ctx, 24, 134, 0xffffff, line);

		snprintf(line, sizeof(line), "Stage 1  %08lX      Entry %08lX",
			(unsigned long) plan->arena,
			(unsigned long) plan->entry);
		printString(ctx, 24, 148, 0xffffff, line);

		snprintf(line, sizeof(line), "Dash end %08lX      Stack %08lX",
			(unsigned long) plan->imageEnd,
			(unsigned long) plan->sp);
		printString(ctx, 24, 162, 0xffffff, line);

		printString(ctx, 16, 186, 0x808080,
			"Reset or power-cycle to return to the dashboard.");

		printString(ctx, 16, ctx->screenHeight - 26, 0x606060,
			CH_PS1_CROSS_BUTTON " Enter    "
			CH_PS1_CIRCLE_BUTTON " Back");

		endFrame(ctx);
	}

	waitForRelease();
	return 1;
}

void runSIOLoader(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
) {
	(void) state;
	(void) item;

	AppLaunchPlan plan;
	AppPlanResult result =
		planEmbeddedApp(sioLoaderExe, SIO_LOADER_ERASE_RAM, &plan);

	if (result != APP_PLAN_OK) {
		showPlanError(ctx, result);
		return;
	}

	if (!confirmSIOLoaderHandoff(ctx, &plan))
		return;

	/* Never returns. */
	runAppLaunch(&plan);
}
