/*
 * PSX-iTests - live system status HUD (RAM/VRAM meter widget)
 *
 * A small always-on corner widget that shows this build's real memory
 * footprint against what the console actually has, so the headroom a bigger
 * RAM chip buys is visible the moment the app boots - no menu digging
 * required.
 *
 * What the numbers mean (deliberately NOT fabricated):
 *   RAM  used  = this program's own static footprint (code + data + bss +
 *                stack reserve), i.e. _bssEnd - RAM_BASE - read straight off
 *                the linker symbols the same way the RAM tester already
 *                does for its "safe to test above this" boundary.
 *   RAM  total = physically detected main RAM (2/8/16 MB), probed once at
 *                boot via detectPhysicalRAMSizeMB() (console_info.h).
 *   VRAM used  = this build's known, fixed VRAM layout: the two 320x240
 *                framebuffers plus the font/background/icon texture atlases,
 *                whose sizes and VRAM addresses are compile-time constants
 *                (see renderer.c / icon.c) - not a live allocator readout,
 *                since the PS1 has no such allocator to query.
 *   VRAM total = standard PS1 VRAM (1 MB), or 2 MB if the RAM/VRAM tester's
 *                "VRAM size" option has been set to test the extended size.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "main/renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Probes physical RAM once (see detectPhysicalRAMSizeMB()) and caches
// everything the HUD needs. Call once at boot, after setupRenderer().
void initSystemHUD(void);

// Draws the compact "RAM: [bar] x.x / y.y MB" / "VRAM: [bar] ..." widget in
// the top-right corner. Cheap - just reads the cached values from
// initSystemHUD(), no re-probing. No-op while the widget is toggled off
// (see toggleSystemHUD()) - off by default, since not everyone wants a
// permanent overlay on their menu.
void drawSystemHUD(RenderContext *ctx);

// The widget starts OFF by default. Toggles it and returns the new state;
// bound to the SELECT button at the top level (see main.c), alongside the
// existing L2/R2 toggles.
bool toggleSystemHUD(void);
bool isSystemHUDEnabled(void);

// Detected physical RAM in whole megabytes (2, 8 or 16), cached at boot.
uint32_t getDetectedRAMMB(void);

// --- Trophy badge conditions (see badge.h for the Trophy Room screen) ---
// Four independent badges. VRAM and SPU are honest stand-ins, not true
// hardware size detection - see the comments on isBadge2MBVRAM()/
// isBadgeSPUVerified() for why.
bool isBadge8MBRAM(void);     // detected physical RAM >= 8 MB
bool isBadge16MBRAM(void);    // detected physical RAM >= 16 MB
bool isBadge2MBVRAM(void);    // VRAM size TESTER OPTION set to 2 MB - there's
                              // no non-destructive VRAM size probe like the
                              // RAM one, so this reflects the user's current
                              // "VRAM size" setting in the RAM/VRAM/SPU
                              // tester, not an auto-detected physical size.
bool isBadgeSPUVerified(void);// SPU RAM test has been run and passed this
                              // session - SPU_RAM_SIZE is a fixed 512 KB on
                              // every PS1 ever made, there's no real
                              // "2 MB SPU RAM" mod to detect, so this badge
                              // is a "verified working" stand-in instead.

// How many of the 4 badges above are currently satisfied (0-4).
int getUnlockedBadgeCount(void);

// Feature 1 unlocks at 1+ badges; Feature 2 (not implemented yet) at 3+.
bool isFeature1Unlocked(void);
bool isFeature2Unlocked(void);

#ifdef __cplusplus
}
#endif
