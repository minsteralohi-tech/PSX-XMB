/*
 * PSX-iTests - Controller (pad) tester
 */

#pragma once

#include "main/renderer.h"
#include "main/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

// Menu callback: shows both controller ports simultaneously, with a visual
// layout modeled on the original PS1 digital pad. Displays live button
// states, analog stick position (if an analog-capable pad is connected)
// and lets the player test the rumble motors (L2 = small motor, R2 = big
// motor on port 1). Exits when Start is pressed on port 1.
void runPadTest(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
);

#ifdef __cplusplus
}
#endif
