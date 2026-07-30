/*
 * PSX-iTests - "Trophy Room" showcase screen (formerly "Mod Badge")
 *
 * Tracks four independent badges tied to real detection/test state
 * elsewhere in the project (see hud.h for the exact conditions):
 *
 *   1. 8 MB RAM   - physical main RAM detected >= 8 MB
 *   2. 16 MB RAM  - physical main RAM detected >= 16 MB
 *   3. 2 MB VRAM  - the RAM/VRAM/SPU tester's "VRAM size" option set to 2 MB
 *                   (there's no non-destructive way to auto-detect physical
 *                   VRAM size the way main RAM is probed, so this reflects
 *                   the user's current setting, not a hardware read)
 *   4. SPU Verified - the SPU RAM test has been run and passed this session
 *                   (there's no real "bigger SPU RAM" mod to detect - SPU
 *                   RAM is a fixed 512 KB on every PS1 - so this is an
 *                   honest "verified working" stand-in for the 4th slot)
 *
 * 1+ badges unlocks Feature 1 (a small celebratory flourish on this screen
 * itself). 3+ badges unlocks Feature 2, which isn't implemented yet - the
 * screen just reports readiness for it.
 */

#pragma once

#include "main/renderer.h"
#include "main/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

void runTrophyRoom(RenderContext *ctx, UIState *state, const MenuItem *item);

#ifdef __cplusplus
}
#endif
