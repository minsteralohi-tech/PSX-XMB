/*
 * PSX-iTests - Music (BGM track) settings screen
 *
 * A standalone picker for the background music track, living under
 * Settings. This used to be the "Music" option inside the Gouraud Waves +
 * Sparkle + PSP theme's customization submenu (Icons/Color/Wave
 * Style/Music) - moved out since BGM selection isn't really specific to
 * that one theme, and more tracks are planned, which will make this list
 * grow independently of that submenu's other three (genuinely
 * theme-specific) options.
 *
 * Up/Down moves the highlight and live-switches the playing track
 * immediately (same instant-preview feel the old in-theme picker had);
 * Circle exits back to Settings.
 */

#pragma once

#include "main/renderer.h"
#include "main/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

void runMusicSettings(RenderContext *ctx, UIState *state, const MenuItem *item);

#ifdef __cplusplus
}
#endif
