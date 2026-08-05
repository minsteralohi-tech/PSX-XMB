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

/*
 * DISABLED BY DEFAULT.
 *
 * The first hardware test of this sequence gave a black screen with the music
 * still playing, and I could not isolate the cause by inspection - the parts
 * I could check statically (VRAM slots, CLUT addresses, packet word counts,
 * texture page bases, display enable order, the controller skip logic) all
 * verify correct. A dashboard that does not boot is worse than one without an
 * intro, so this ships off.
 *
 * TO BISECT IT, in this order:
 *
 *   1. Set PS1_BOOT_ENABLED to 1, leave PS1_BOOT_TEXTURES at 0.
 *      That runs the sequence using ONLY flat polygons - the white screen,
 *      the orange diamond and the two triangles - and uploads no textures.
 *
 *        - White screen with the diamond  -> the polygon path is fine and the
 *          fault is in the texture upload or the sprite drawing.
 *        - Still black                    -> the fault is before any of that,
 *          in the loop or the frame setup itself.
 *
 *   2. If step 1 showed the diamond, set PS1_BOOT_TEXTURES to 1 as well. A
 *      black screen then points at initPS1Boot()'s uploads; garbled or
 *      missing logos point at the sprite() UV/CLUT setup.
 *
 * That single build distinguishes the three candidates without a debugger.
 */
#define PS1_BOOT_ENABLED  1

/*
 * Draw and upload the four logo/wordmark textures. Set to 0 to run the
 * sequence as flat polygons only - see the bisect note above.
 */
#define PS1_BOOT_TEXTURES 1

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
