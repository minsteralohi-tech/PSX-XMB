/*
 * ps1-ram-tester - (C) 2026 spicyjpeg
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include "common/gpu.h"
#include "common/sio0.h"
#include "common/spu.h"
#include "main/defs.h"
#include "main/font.h"
#include "main/hud.h"
#include "main/icon.h"
#include "main/gameid.h"
#include "main/mainmenu.h"
#include "main/renderer.h"
#include "main/sound.h"
#include "main/ui.h"
#include "main/xmb_bg.h"
#include "main/xmb_menu.h"
#include "ps1/gpucmd.h"
#include "ps1/registers.h"

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

int main(int argc, const char **argv) {
	(void) argc;
	(void) argv;

#ifdef ENABLE_LOGGING
	initSerialIO(115200);
#endif
	LOG("PSX-iTests " VERSION_STRING " (" __DATE__ " " __TIME__ ")");

	// The dashboard no longer installs a serial TTY device.
	//
	// It used to delete the kernel's "tty" by name and install its own
	// replacement, the way UniROM does. The problem is that the DCB and every
	// handler it points at live inside this image at ~0x8001xxxx, and the
	// kernel's device table keeps pointing at them after we hand the console
	// to another program. A target that loads low - the 240p Test Suite loads
	// at 0x80010000 - overwrites those handlers, and the next BIOS call that
	// touches "tty" jumps into the middle of the launched program.
	//
	// Removing the device at hand-off time was worse: RemoveDevice invokes the
	// driver's own deinit handler, which writes to SIO1 and spins forever
	// waiting for TX-ready when nothing is plugged in, so the hand-off froze
	// before it had even started.
	//
	// Serial is now the standalone SIO loader's job, so nothing here needs the
	// redirect. Leaving the kernel's own TTY untouched means there is nothing
	// dangling to clean up and no BIOS call to block on.

	if ((GPU_GP1 & GP1_STAT_FB_MODE_BITMASK) == GP1_STAT_FB_MODE_PAL) {
		LOG("using PAL mode");
		setupGPU(GP1_MODE_PAL, SCREEN_WIDTH, SCREEN_HEIGHT);
	} else {
		LOG("using NTSC mode");
		setupGPU(GP1_MODE_NTSC, SCREEN_WIDTH, SCREEN_HEIGHT);
	}

	initSPU();
	initSound();
	playBGM();
	initControllerBus();

	static RenderContext ctx;
	static UIState       state;

	setupRenderer(&ctx, SCREEN_WIDTH, SCREEN_HEIGHT);
	setupUIState(&state);
	initIcons(&ctx);
	initNebulaTexture(&ctx);
	initXMB();
	initSystemHUD();
	enterMainMenu(&ctx, &state, 0);

#if GAMEID_ENABLED
	// Buffer for SYSTEM.CNF. Static rather than a local: 2 KB is far more
	// than this stack wants to carry.
	static uint8_t gameIdScratch[GAMEID_SCRATCH_SIZE];
#endif

#if GAMEID_ENABLED
	gameIdInit();
#endif

	for (;;) {
		uint16_t buttons = pollController(0) | pollController(1);

#if GAMEID_ENABLED
		gameIdPoll(gameIdScratch, sizeof(gameIdScratch));
#endif

		beginFrame(&ctx);

		if (isXMBActive()) {
			renderXMB(&ctx, &state);
			updateXMB(&ctx, &state, buttons);
		} else {
			renderMenu(&ctx, &state);
			updateMenu(&ctx, &state, buttons);
		}

		// L2/R2 toggle background music / the scrolling background graphic;
		// SELECT toggles the RAM/VRAM HUD widget (off by default) - all
		// available both on the classic main menu and on the XMB top level.
		if (isMainMenuActive(&state) || isXMBActive()) {
			drawSystemHUD(&ctx);

			if (state.buttonsPressed & PAD_BTN_L2)
				toggleBGM();
			if (state.buttonsPressed & PAD_BTN_R2)
				setBackgroundScrollEnabled(!isBackgroundScrollEnabled());
			if (state.buttonsPressed & PAD_BTN_SELECT)
				toggleSystemHUD();

			// R1 rescans the disc. Manual rather than automatic: reading it
			// costs a visible pause, and detecting the lid automatically
			// would mean driving the CD-ROM behind the BIOS's back, which
			// crashes the dashboard - see the note in gameid.c.
#if GAMEID_ENABLED
			if (state.buttonsPressed & PAD_BTN_R1)
				gameIdRequestScan();
#endif

			if (!isXMBActive()) {
				char toggleLine[64];
				snprintf(
					toggleLine, sizeof(toggleLine),
					"L2: Music %s   R2: Background scroll %s",
					isBGMEnabled() ? "ON" : "OFF",
					isBackgroundScrollEnabled() ? "ON" : "OFF"
				);
				printString(&ctx, 16, 196, 0x505050, toggleLine);

				char toggleLine2[48];
				snprintf(
					toggleLine2, sizeof(toggleLine2),
					"SELECT: System HUD %s",
					isSystemHUDEnabled() ? "ON" : "OFF"
				);
				printString(&ctx, 16, 206, 0x505050, toggleLine2);
			}
		}

#if GAMEID_ENABLED
		drawGameIdNotice(&ctx);
#endif

		endFrame(&ctx);
	}

	return 0;
}
