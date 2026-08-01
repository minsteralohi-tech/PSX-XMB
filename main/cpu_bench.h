/*
 * PSX-iTests - CPU benchmark
 */

#pragma once

#include "main/renderer.h"
#include "main/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

extern char cpuScoreResult[32];

// Menu callback: runs a fixed CPU-bound workload, timed against actual
// vblank periods (so the result is frame-accurate regardless of NTSC/PAL),
// and reports an iterations-per-second score before returning to the menu.
void runCPUBenchmark(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
);

#ifdef __cplusplus
}
#endif
