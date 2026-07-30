/*
 * PSX-iTests - RAM/VRAM/SPU RAM tester submenu
 *
 * Ported from spicyjpeg's ps1-ram-tester, adapted to run as a submenu of
 * PSX-iTests rather than as the top-level menu. Renamed from the original
 * enterMainMenu() to enterRAMTesterMenu() to avoid colliding with our own
 * top-level main/mainmenu.h, which already defines enterMainMenu() for the
 * PSX-iTests root menu.
 */

#pragma once

#include "main/renderer.h"
#include "main/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t vramSize;
extern uint8_t testPasses;

// True once the SPU RAM test has been run and passed this session. There is
// no real hardware equivalent of a "bigger SPU RAM" mod (SPU_RAM_SIZE is a
// fixed 512 KB on every PS1 ever made) - this is used as the 4th Trophy
// badge condition as an honest stand-in ("SPU Verified"), not a size claim.
bool isSPURAMTestPassed(void);

// True if `menu` is this submenu's own item list. Used by ui.c's renderMenu()
// to decide whether the live theme background is safe to draw here - see
// isHeavyBackgroundUnsafe() there for why it isn't, for this screen family.
bool isRAMTesterMenu(const MenuItem *menu);

void enterRAMTesterMenu(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
);

#ifdef __cplusplus
}
#endif
