/*
 * PSX-iTests - CD music player
 */

#pragma once

#include "main/renderer.h"
#include "main/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

// Menu callback: a basic CD-DA (audio track) player, similar in spirit to
// the BIOS's own built-in music player, using our own font/UI theme.
// Note: PSX-iTests' own disc has no audio tracks - this only does
// anything useful once you swap in an actual audio CD or multi-track
// game disc after booting. Exits via START+SELECT together.
void runCDPlayer(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
);

#ifdef __cplusplus
}
#endif
