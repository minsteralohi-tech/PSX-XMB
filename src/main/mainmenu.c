/*
 * PSX-iTests - (C) 2026
 *
 * Built on ps1-bare-metal and ps1-ram-tester's UI framework by spicyjpeg,
 * licensed under the MIT license.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "common/reboot.h"
#include "main/cpu_bench.h"
#include "main/cd_player.h"
#include "main/console_info.h"
#include "main/fastboot.h"
#include "main/memcard.h"
#include "main/gpu_colorbars.h"
#include "main/gpu_cube.h"
#include "main/mainmenu.h"
#include "main/modals.h"
#include "main/pad_test.h"
#include "main/ramtester.h"
#include "main/renderer.h"
#include "main/spu_channel_test.h"
#include "main/ui.h"
#include "main/xmb_bg.h"
#include "main/xmb_menu.h"

/* Fast Boot: chain-load the embedded loader (see fastboot.c). Never returns. */
static void doFastBoot(RenderContext *ctx, UIState *state, const MenuItem *item) {
	(void) ctx;
	(void) state;
	(void) item;
	launchLoader();
}

/* Main menu */

static const MenuItem mainMenu[] = {
	{
		.name = "- PSX-iTests v1.0 -",
		.type = ITEM_TITLE
	}, {
		.type = ITEM_SEPARATOR
	}, {
		.name     = "Background theme",
		.type     = ITEM_ENUM,
		.minValue = 0,
		.maxValue = XMB_THEME_COUNT - 1,
		.enum_    = {
			.value = &xmbThemeIndex,
			.items = xmbThemeNames
		}
	}, {
		.type = ITEM_SEPARATOR
	}, {
		.name   = "GPU: Color bar test pattern",
		.type   = ITEM_ACTION,
		.action = { .callback = runColorBarTest }
	}, {
		.name   = "GPU: Spinning cube (3D/GTE)",
		.type   = ITEM_ACTION,
		.action = { .callback = runGPUCubeTest }
	}, {
		.name     = "CPU: Benchmark",
		.type     = ITEM_ACTION,
		.action   = {
			.tag      = cpuScoreResult,
			.callback = runCPUBenchmark
		}
	}, {
		.name   = "SPU: Channel test",
		.type   = ITEM_ACTION,
		.action = { .callback = runSPUChannelTest }
	}, {
		.name   = "Controller (pad) test",
		.type   = ITEM_ACTION,
		.action = { .callback = runPadTest }
	}, {
		.name   = "Console Information",
		.type   = ITEM_ACTION,
		.action = { .callback = runConsoleInfo }
	}, {
		.name   = "Tools",
		.type   = ITEM_ACTION,
		.action = { .callback = enterToolsMenu }
	}, {
		.type = ITEM_SEPARATOR
	}, {
		.name   = "PS1 RAM Tester v1.0.0 by spicyjpeg",
		.type   = ITEM_ACTION,
		.action = { .callback = enterRAMTesterMenu }
	}, {
		.type = ITEM_SEPARATOR
	}, {
		.name   = "Boot CD-ROM",
		.type   = ITEM_ACTION,
		.action = { .callback = enterFastRebootMenu }
	}, {
		.name   = "Game Disc (Fast Boot)",
		.type   = ITEM_ACTION,
		.action = { .callback = doFastBoot }
	}, {
		.name   = "Reboot system",
		.type   = ITEM_ACTION,
		.action = { .callback = doFullReboot }
	}, {
		.type = ITEM_SEPARATOR
	}, {
		.name   = "About this tool...",
		.type   = ITEM_ACTION,
		.action = { .callback = enterAboutMenu }
	}, {
		.type = ITEM_END
	}
};

void enterMainMenu(RenderContext *ctx, UIState *state, const MenuItem *item) {
	(void) ctx;
	(void) item;

	state->currentMenu = mainMenu;
	state->menuCursor  = findFirstSelectableItem(mainMenu);

	// The XMB cross-menu is the real top level; returning "to the main menu"
	// from any submenu or test brings the XMB back up.
	setXMBActive(true);
}

bool isMainMenuActive(const UIState *state) {
	return state->currentMenu == mainMenu;
}

/* Tools submenu */

static const MenuItem toolsMenu[] = {
	{
		.name = "- Tools -",
		.type = ITEM_TITLE
	}, {
		.type = ITEM_SEPARATOR
	}, {
		.name   = "Memory Card Manager",
		.type   = ITEM_ACTION,
		.action = { .callback = runMemoryCardManager }
	}, {
		.name   = "CD Music Player",
		.type   = ITEM_ACTION,
		.action = { .callback = runCDPlayer }
	}, {
		.type = ITEM_SEPARATOR
	}, {
		.name   = "Back to main menu",
		.type   = ITEM_ACTION,
		.action = { .callback = enterMainMenu }
	}, {
		.type = ITEM_END
	}
};

void enterToolsMenu(RenderContext *ctx, UIState *state, const MenuItem *item) {
	(void) ctx;
	(void) item;

	state->currentMenu = toolsMenu;
	state->menuCursor  = findFirstSelectableItem(toolsMenu);
}
