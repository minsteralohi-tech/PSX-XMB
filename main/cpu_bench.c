/*
 * PSX-iTests - CPU benchmark
 *
 * Runs a fixed, deterministic CPU-bound workload (a simple LCG-style integer
 * churn - nothing fancy, just enough to keep the CPU busy and be hard for
 * the compiler to optimize away thanks to the volatile accumulator) and
 * times it against actual vblank periods via the existing beginFrame/
 * renderProgressScreen/endFrame cycle already used by the RAM tests. This
 * gives a frame-accurate elapsed time without touching the Timer registers
 * directly, and correctly accounts for NTSC vs PAL refresh rate.
 */

#include <stdint.h>
#include <stdio.h>
#include "main/cpu_bench.h"
#include "main/mainmenu.h"
#include "ps1/gpucmd.h"
#include "ps1/registers.h"

char cpuScoreResult[32] = "";

#define TOTAL_ITERATIONS     20000000u
#define ITERATIONS_PER_FRAME 20000u

void runCPUBenchmark(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
) {
	(void) state;
	(void) item;

	int refreshRate =
		((GPU_GP1 & GP1_STAT_FB_MODE_BITMASK) == GP1_STAT_FB_MODE_PAL)
			? 50 : 60;

	volatile uint32_t x      = 0x12345678;
	uint32_t          frames = 0;

	for (uint32_t i = 0; i < TOTAL_ITERATIONS; i++) {
		// Simple LCG-style churn - the actual computation doesn't matter,
		// it just needs to be CPU-bound and hard to optimize away (hence
		// the volatile accumulator).
		x = x * 1103515245u + 12345u;

		if ((i % ITERATIONS_PER_FRAME) == 0) {
			beginFrame(ctx);
			renderProgressScreen(
				ctx, i, TOTAL_ITERATIONS, "Running CPU benchmark..."
			);
			endFrame(ctx); // waits for vblank internally - our frame clock
			frames++;
		}
	}

	uint32_t elapsedMillis = (frames * 1000u) / (uint32_t) refreshRate;

	// Division before multiplication to avoid needing 64-bit arithmetic
	// (TOTAL_ITERATIONS * 1000 would overflow 32 bits) - trades a small
	// amount of precision for staying entirely in native 32-bit ops, which
	// is fine here since this is a fun benchmark number, not a scientific
	// measurement.
	uint32_t score = elapsedMillis
		? (TOTAL_ITERATIONS / elapsedMillis) * 1000u
		: 0;

	snprintf(cpuScoreResult, sizeof(cpuScoreResult), "%u iter/sec", score);

	// Deliberately no enterMainMenu() call here - state->currentMenu and
	// state->menuCursor were never touched anywhere in this function, so
	// they still correctly point at whichever item was selected to get
	// here.
}
