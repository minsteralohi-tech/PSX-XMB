/*
 * PSX-iTests - PlayStation boot sequence (see intro_ps1.c)
 *
 * A recreation of the original PS1 startup: the white SCE screen with its
 * orange diamond, then the black PlayStation logo screen. Timed from the
 * HTML remake in PS1_Startup_Remake-1.0.0, starting from white (the demo's
 * opening black-to-white fade is not part of the sequence proper).
 */

#pragma once

#include "main/renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set to 0 to compile the boot sequence out entirely. */
#define PS1_BOOT_ENABLED 1

/* Upload the four logo/text textures. Call once, after the GPU is up. */
void initPS1Boot(RenderContext *ctx);

/*
 * Play the sequence, about 15.65 seconds. Returns when it finishes or the
 * user presses anything.
 */
void runPS1Boot(RenderContext *ctx);

#ifdef __cplusplus
}
#endif
