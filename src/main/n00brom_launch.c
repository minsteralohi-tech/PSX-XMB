/*
 * PSX-iTests - n00bROM integration (see n00brom_launch.h for the rationale
 * behind showing info instead of hot-launching the cart ROM).
 */

#include <stdint.h>
#include "common/sio0.h"
#include "main/defs.h"
#include "main/font.h"
#include "main/n00brom_launch.h"
#include "main/renderer.h"
#include "main/xmb_bg.h"

// The n00brom.rom cartridge image, embedded via addBinaryFile() in
// CMakeLists.txt. Referenced here so the linker keeps it in the build even
// though we deliberately don't jump into it (see the header).
extern const uint8_t n00bromRom[];

void runN00bROMInfo(RenderContext *ctx, UIState *state, const MenuItem *item) {
	(void) state;
	(void) item;

	// Touch the embedded ROM so it's linked in and the symbol resolves.
	volatile uint8_t sig = n00bromRom[4];   // 'n' of the "n00b" cart signature
	(void) sig;

	// Debounce the button that opened this screen.
	while (pollController(0) | pollController(1))
		;

	for (;;) {
		if (pollController(0) | pollController(1))
			break;

		beginFrame(ctx);
		drawXMBBackground(ctx);

		printString(ctx, 16, 20, 0xffffff, "n00bROM");

		printString(ctx, 16,  48, 0x808080,
			"n00bROM (by Lameguy64) is a homebrew");
		printString(ctx, 16,  60, 0x808080,
			"debug / cheat cartridge ROM.");

		printString(ctx, 16,  84, 0xc0c0c0,
			"The ROM image is bundled with this build");
		printString(ctx, 16,  96, 0xc0c0c0,
			"(assets/n00brom.rom) ready to flash to a");
		printString(ctx, 16, 108, 0xc0c0c0,
			"compatible cheat cartridge.");

		printString(ctx, 16, 132, 0x808080,
			"It is a cartridge ROM, not a PS-EXE, so it");
		printString(ctx, 16, 144, 0x808080,
			"can't be launched safely from RAM here. To");
		printString(ctx, 16, 156, 0x808080,
			"run it directly, chainload an official");
		printString(ctx, 16, 168, 0x808080,
			"n00bROM PS-EXE (like this menu boots discs).");

		printString(ctx, 16, 210, 0x606060,
			CH_PS1_CIRCLE_BUTTON " Back");

		endFrame(ctx);
	}

	// Debounce on the way out so the outer XMB doesn't re-read the press.
	while (pollController(0) | pollController(1))
		;
}
