/*
 * PSX-iTests - boot intro (see intro.c)
 *
 * A five-second startup animation driven by the existing Gouraud wave
 * renderer, shown once at launch before the menu appears.
 */

#pragma once

#include "main/renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Set to 0 to skip the intro entirely at build time - useful when iterating
 * on the dashboard itself, where sitting through it on every boot gets old.
 */
#define INTRO_ENABLED 1

/*
 * Run the boot intro. Returns when it finishes or the user presses anything.
 *
 * Must be called AFTER the GPU, textures and sound are up (it draws through
 * the normal renderer and uses the wave theme's textures) and BEFORE the menu
 * loop starts.
 */
void runBootIntro(RenderContext *ctx);

#ifdef __cplusplus
}
#endif
