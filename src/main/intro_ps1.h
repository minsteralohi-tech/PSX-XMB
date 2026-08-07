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
 * The black screen this used to give was a truncated DMA upload: three of
 * the four textures had dimensions whose transfer length was not a whole
 * number of 16-word chunks, so sendVRAMData() sent short and left the GPU
 * parked mid-vramWrite forever. See the note on the size constants in
 * intro_ps1.c. Fixed by padding the artwork.
 *
 * THE BISECT SWITCH BELOW IS KEPT, since it is what found that:
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

/*
 * Draw the PlayStation screen's logo as a selectable full 3D model
 * (ps_logo_bios*.h, GTE-transformed) instead of the flat intro_pslogo.png
 * sprite, and swing it into place as it fades in.
 *
 * Set to 0 to go back to the sprite. Kept as a switch rather than deleting the
 * sprite path because the model's on-screen pose is tuned by eye - see the
 * PS_LOGO_* constants in intro_ps1.c - and having the old look one rebuild
 * away makes comparing them trivial.
 */
#define PS1_BOOT_LOGO_3D 1

/*
 * Which look the SCE screen uses. A menu at boot picks one, so all three can
 * be compared on hardware from a single build - scaffolding for choosing a
 * direction, not a permanent feature.
 */
typedef enum {
	PS1_INTRO_CLASSIC = 0,   /* flat gradient, exactly like the original    */
	PS1_INTRO_GLASS,         /* the same mark drawn semi-transparent        */
	PS1_INTRO_SPIN,          /* the same flat mark, spun once               */
	PS1_INTRO_WHITE_RIBBONS, /* solid mark over white ribbons + sparkles    */
	PS1_INTRO_COUNT
} PS1IntroVariant;

/* Set to 0 once a direction is settled on. */
#define PS1_BOOT_SHOW_MENU 1

/* Show the variant picker and return the chosen one. */
int chooseIntroVariant(RenderContext *ctx);

/* Select the look used by runPS1Boot(). */
void setPS1IntroVariant(int variant);

/* Upload the four logo/text textures. Call once, after the GPU is up. */
void initPS1Boot(RenderContext *ctx);

/* Permanent hardware-tool entry for tuning the 3D logo pose, shadows,
 * backdrop effects and material animations without replaying the boot menu. */
void runPSLogoPoseTool(RenderContext *ctx);

/*
 * Play the sequence, about 15.65 seconds. Returns when it finishes or the
 * user presses anything.
 */
void runPS1Boot(RenderContext *ctx);

#ifdef __cplusplus
}
#endif
