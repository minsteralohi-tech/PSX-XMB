/*
 * PSX-iTests - SPU channel test
 */

#pragma once

#include "main/renderer.h"
#include "main/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

// Menu callback: lets the player step through all 24 SPU channels
// individually, playing a short test tone on whichever one is selected, to
// confirm each channel actually outputs sound. Restores normal scroll/
// confirm/BGM playback on exit, since testing channels 0-2 interrupts them.
void runSPUChannelTest(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
);

#ifdef __cplusplus
}
#endif
