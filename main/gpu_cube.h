/*
 * PSX-iTests - GPU 3D (GTE) test
 */

#pragma once

#include "main/renderer.h"
#include "main/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

// Menu callback: displays a spinning, GTE-rendered cube until any button is
// pressed, then returns to the main menu. Proves the GTE/3D pipeline works,
// as opposed to the flat 2D color bar test.
void runGPUCubeTest(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
);

#ifdef __cplusplus
}
#endif
