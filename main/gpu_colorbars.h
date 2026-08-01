/*
 * PSX-iTests - GPU color bar test pattern
 */

#pragma once

#include "main/renderer.h"
#include "main/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

// Menu callback: displays a full-screen color bar test pattern until any
// button is pressed, then returns to the main menu.
void runColorBarTest(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
);

#ifdef __cplusplus
}
#endif
