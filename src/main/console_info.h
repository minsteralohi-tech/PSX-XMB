/*
 * PSX-iTests - Console information (BIOS/motherboard/GPU/RAM detection)
 */

#pragma once

#include <stddef.h>
#include "main/renderer.h"
#include "main/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns how much of the currently-configured main RAM is actually backed by
// real, distinct memory (2, 8 or 16 MB), never exceeding configuredBytes.
// Non-destructive: it does NOT modify DRAM_CTRL and uses only the safe
// six-point distinct-region probe. The RAM tester uses this to clamp its
// destructive pattern test so it can never hammer "phantom" addresses that
// aren't physically present (which hangs real hardware).
size_t probePhysicalMainRAMSize(size_t configuredBytes);

// Runs the same automatic progressive probe the Console Information page
// uses (2/8/16 MB) and returns the result in whole megabytes. Non-destructive
// in its net effect: it temporarily reconfigures DRAM_CTRL to test each
// tier, then always restores the original value before returning, so it's
// safe to call repeatedly (e.g. once at boot for the HUD widget).
uint32_t detectPhysicalRAMSizeMB(void);

// Menu callback: the combined console information page. Gathers and displays,
// in one screen:
//
//   BIOS              - date, region, version string
//   CD-ROM Controller - Mechacon version bytes and matched motherboard
//   RAM               - detected size (2/8/16 MB) and what that implies
//   GPU Version       - 160-pin vs 208-pin, plus the motherboard model
//
// All readings are taken automatically when the page opens; nothing needs to
// be triggered by hand. Exits on any button press.
//
// This replaces the former separate runBiosMemoryDump(), runCDROMTest(),
// runRAMTest() and runGPUVersionTest() screens.
void runConsoleInfo(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
);

#ifdef __cplusplus
}
#endif
