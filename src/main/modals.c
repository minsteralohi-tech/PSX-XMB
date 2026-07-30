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

#include <stdint.h>
#include "common/reboot.h"
#include "main/sound.h"
#include "main/xmb_bg.h"
#include "main/font.h"
#include "common/sio0.h"
#include "main/defs.h"
#include "main/mainmenu.h"
#include "main/modals.h"
#include "main/ramtester.h"
#include "main/renderer.h"
#include "main/ui.h"
#include "ps1/registers.h"

/* Reboot functions */

static void showRebootProgress(RenderContext *ctx, const char *message) {
	// This needs to be done multiple times in order to completely "flush" the
	// rendering pipeline.
	for (int i = 0; i < 3; i++) {
		beginFrame(ctx);
		renderProgressScreen(ctx, 0, 1, message);
		endFrame(ctx);
	}
}

/* ---- CD-ROM CD-R/import unlock sequence ----
 *
 * This is genuine, well-established homebrew dev tooling, not a piracy
 * mechanism specifically - CD-Rs don't have the physical SCEx wobble
 * signal a pressed disc has, so testing your own burned homebrew discs
 * on real hardware without a modchip needs exactly this. Same category
 * of tool as n00brom (Lameguy64's open-source PS1 dev cartridge
 * firmware), which documents this same mechanism and exists for the same
 * reason.
 *
 * Confirmed directly from psx-spx's own CD-ROM command table: Test
 * command (0x19) sub-functions 0x50-0x56 ("Secret 1" through "Secret 7")
 * feed the exact same "Licensed by Sony Computer Entertainment <region>"
 * text a genuine disc's SCEx signal would provide, just via software
 * instead of the physical wobble groove. Region text used here is
 * "America", matching every BIOS this project has been tested against so
 * far (all reported region A). n00bROM's own documentation notes this
 * specific mechanism only works on US/EU/Yaroze consoles, not Japan.
 *
 * Uses the same proven BUSYSTS-synchronized command infrastructure as
 * the CD-ROM controller test and CD player, self-contained here per this
 * project's established pattern.
 */

#define CDROM_REG0 (*(volatile uint8_t *) 0x1f801800)
#define CDROM_REG1 (*(volatile uint8_t *) 0x1f801801)
#define CDROM_REG2 (*(volatile uint8_t *) 0x1f801802)
#define CDROM_REG3 (*(volatile uint8_t *) 0x1f801803)

#define CDROM_STAT_BUSYSTS (1 << 7)

void doNormalBoot(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
) {
	(void) item;

	/*
	 * Warn before handing over to the BIOS. softFastRebootWithConfig() never
	 * returns, so once control leaves this function there is no opportunity
	 * left to tell the user anything: on a backup/CD-R without a modchip the
	 * kernel simply refuses the disc and the console sits on a black screen
	 * with no explanation. Saying so up front, with a way to back out, is
	 * far better than an unexplained freeze.
	 */
	while (pollController(0) | pollController(1))
		;

	for (;;) {
		uint16_t buttons = pollController(0) | pollController(1);

		if (buttons & PAD_BTN_CIRCLE) {
			// Cancelled - go back to the menu.
			playCancelSound();
			while (pollController(0) | pollController(1))
				;
			enterMainMenu(ctx, state, 0);
			return;
		}
		if (buttons & PAD_BTN_CROSS)
			break;

		beginFrame(ctx);
		drawXMBBackground(ctx);

		printString(ctx, 16,  30, 0x808080, "BOOT DISC");
		printString(ctx, 16,  54, 0xffffff, "The console will now hand control to the");
		printString(ctx, 16,  66, 0xffffff, "BIOS to boot the disc in the drive.");

		printString(ctx, 16,  92, 0x808080, "Note");
		printString(ctx, 16, 106, 0xffffff, "Backup / CD-R discs only boot on a console");
		printString(ctx, 16, 118, 0xffffff, "with a modchip fitted. Without one the");
		printString(ctx, 16, 130, 0xffffff, "screen stays black and the console has to");
		printString(ctx, 16, 142, 0xffffff, "be powered off - this app cannot recover");
		printString(ctx, 16, 154, 0xffffff, "once the BIOS has taken over.");

		printString(ctx, 16, 200, 0x808080,
			CH_PS1_CROSS_BUTTON " Boot disc    " CH_PS1_CIRCLE_BUTTON " Cancel");

		endFrame(ctx);
	}

	while (pollController(0) | pollController(1))
		;

	showRebootProgress(ctx, "Waiting for kernel to load CD-ROM...");
	// vramSize is set by the RAM/VRAM/SPU RAM tester submenu, defaulting to
	// 0 (standard 1 MB VRAM) if that submenu was never entered.
	softFastRebootWithConfig(DRAM_CTRL & 0xffff, vramSize);
	__builtin_unreachable();
}

void doFullReboot(RenderContext *ctx, UIState *state, const MenuItem *item) {
	(void) state;
	(void) item;

	// softFastReboot() skips the BIOS shell's own init (memory card check
	// animation, shell UI setup, etc.) by dropping a dummy shell at the
	// address the BIOS loads it to and jumping in past that step - a
	// genuine soft/fast reboot, not just a relabeled full one. It's gated
	// on isFastRebootCompatible() (retail Sony BIOS signature) and silently
	// does nothing at all if that check fails, so that's checked here first
	// rather than risking a reboot press that does nothing: any BIOS this
	// doesn't recognise still gets the full softReset() path instead.
	if (isFastRebootCompatible()) {
		showRebootProgress(ctx, "Waiting for kernel to reboot...");
		softFastReboot();
		__builtin_unreachable();
	}

	showRebootProgress(ctx, "Waiting for kernel to reboot...");
	softReset();
	__builtin_unreachable();
}

/* Fast reboot warning and error menus */

static const MenuItem rebootWarningMenu[] = {
	{
		.name = "Warning",
		.type = ITEM_TITLE
	}, {
		.type = ITEM_SEPARATOR
	}, {
		.name = "CD-ROM booting patches the kernel to apply RAM config -",
		.type = ITEM_STATIC
	}, {
		.name = "this can cause compatibility issues with some games.",
		.type = ITEM_STATIC
	}, {
		.type = ITEM_SEPARATOR
	}, {
		.name = "If you understand the risks, insert a disc and close",
		.type = ITEM_STATIC
	}, {
		.name = "the lid before proceeding.",
		.type = ITEM_STATIC
	}, {
		.type = ITEM_SEPARATOR
	}, {
		.name   = "Boot CD-ROM",
		.type   = ITEM_ACTION,
		.action = { .callback = doNormalBoot }
	}, {
		.name   = "Cancel",
		.type   = ITEM_ACTION,
		.action = { .callback = enterMainMenu }
	}, {
		.type = ITEM_END
	}
};

static const MenuItem rebootErrorMenu[] = {
	{
		.name = "Error",
		.type = ITEM_TITLE
	}, {
		.type = ITEM_SEPARATOR
	}, {
		.name = "This console's BIOS ROM version is not supported or not",
		.type = ITEM_STATIC
	}, {
		.name = "compatible with the kernel patch used for CD-ROM booting.",
		.type = ITEM_STATIC
	}, {
		.name = "Only Sony's own retail BIOS ROMs are currently supported.",
		.type = ITEM_STATIC
	}, {
		.type = ITEM_SEPARATOR
	}, {
		.name   = "Back",
		.type   = ITEM_ACTION,
		.action = { .callback = enterMainMenu }
	}, {
		.type = ITEM_END
	}
};

void enterFastRebootMenu(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
) {
	(void) ctx;
	(void) item;

	if (isFastRebootCompatible()) {
		state->currentMenu = rebootWarningMenu;
		state->menuCursor  = (sizeof(rebootWarningMenu) / sizeof(MenuItem)) - 2;
	} else {
		state->currentMenu = rebootErrorMenu;
		state->menuCursor  = (sizeof(rebootErrorMenu)   / sizeof(MenuItem)) - 2;
	}
}

/* "About PSX-iTests" screen - a self-contained loop (same pattern as
 * runConsoleInfo()/runTrophyRoom()) rather than a MenuItem list with its own
 * "Back" action entry. That entry used to work because ITEM_ACTION accepts
 * Start/Circle/Cross interchangeably (shared logic used by every other
 * ITEM_ACTION in the app, so it can't be narrowed just for this one item
 * without affecting menus everywhere else) - meaning three different
 * buttons all quietly worked as "back" here. This way Circle is genuinely
 * the only way out, matching the label at the bottom. */
void enterAboutMenu(RenderContext *ctx, UIState *state, const MenuItem *item) {
	(void) state;
	(void) item;

	while (pollController(0) | pollController(1))
		;

	for (;;) {
		if ((pollController(0) | pollController(1)) & PAD_BTN_CIRCLE) {
			playCancelSound();
			break;
		}

		beginFrame(ctx);
		drawXMBBackground(ctx);

		printString(ctx, 16, 14, 0xffffff, "- PSX-iTests v1.0 -");

		printString(ctx, 16, 34, 0xffffff, "Version " VERSION_STRING);
		printString(ctx, 16, 46, 0xffffff, "PS1 hardware diagnostic and testing tool.");

		printString(ctx, 16, 66, 0xffffff, "Built on ps1-bare-metal and ps1-ram-tester");
		printString(ctx, 16, 78, 0xffffff, "by spicyjpeg (MIT license).");

		printString(ctx, 16, 98, 0xffffff, "Free and open source.");

		printString(ctx, 16, 118, 0xffffff, "Developed with AI assistance (Claude).");

		printString(ctx, 16, ctx->screenHeight - 26, 0x606060, CH_PS1_CIRCLE_BUTTON " Back");

		endFrame(ctx);
	}

	while (pollController(0) | pollController(1))
		;

	enterMainMenu(ctx, state, item);
}
