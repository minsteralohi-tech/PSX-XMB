/*
 * PSX-iTests - XMB-style wave background (drop-in for ps1-bare-metal)
 *
 * Provides a PS3/PSP-XMB-inspired animated "wave" backdrop rendered with
 * Gouraud-shaded quads and additive blending. No texture upload, no FPU:
 * motion comes from the existing integer sine LUT (isin() in trig.c), and
 * the glow is real GPU additive blending (GP0_BLEND_ADD).
 *
 * Usage: call drawXMBBackground(ctx) where drawBackground(ctx) is currently
 * called (in ui.c renderMenu). It honours the existing R2 scroll toggle via
 * isBackgroundScrollEnabled(), so no other wiring is needed.
 *
 * MIT / ISC, same as the rest of the project.
 */

#pragma once

#include <stdint.h>
#include "main/renderer.h"

typedef enum {
	XMB_BG_GOURAUD_WAVES   = 0, // authentic XMB: 3 sine-displaced glowing strips
	XMB_BG_PARALLAX        = 1, // several scrolling additive ribbons (easy)
	XMB_BG_AURORA          = 2, // vertical wash + crossing light ribbons (PS3-ish)
	XMB_BG_COSMOS          = 3, // PS2-intro: nebula, fake cubes, long-trail stars
	XMB_BG_PS5             = 4, // PS5-ish: light ray + drifting warm sparkles
	XMB_BG_GOURAUD_SPARKLE = 5, // Gouraud waves + subtle PS3 sparkles
	XMB_BG_PS5_SPOTLIGHT   = 6, // sweeping spotlight ray + soft bokeh sparkles
	XMB_BG_COSMOS_3D_PP    = 7, // 3D++1: refraction glow when stars pass behind cubes
	XMB_BG_COSMOS_3D_PP2   = 8, // 3D++2: as 3D++1 plus more drifting 2D cubes
	XMB_BG_GOURAUD_PSP     = 9, // PSP XMB: 40-deg tilted ribbon, 2-target morph
	XMB_BG_PSP_BEND        = 10, // PSP "Blue Bend wave": tall, deep-blue fold
	XMB_BG_PSP_THIN        = 11, // PSP "Blue & Green Thin": slim blue-green bands
	XMB_BG_NEBULA2         = 12, // Nebula: textured (lat/long-mapped) hero planet
	XMB_BG_NEBULA3         = 13, // Nebula 2 + writhing corona flares, an
	                              // atmosphere ring, depth-parallax stars, and
	                              // textured roaming planets
	XMB_BG_PS4             = 14, // Flowing silk ribbons on deep blue, PS4
	                              // dynamic-wallpaper style
	XMB_BG_PS4_V2          = 15, // Drifting triangle/circle/cross/square
	                              // outline glyphs, PS4 "Shapes" style
	XMB_BG_TEST_LOGO       = 16  // Rotating GTE-transformed PS-logo model
	                              // on a solid black backdrop
} XMBBgStyle;

/*
 * Themes are the user-facing subset of styles, in the order they appear in
 * the menu. Add or reorder entries in xmb_bg.c to change what the theme
 * picker offers. The menu's ITEM_ENUM points its value at xmbThemeIndex and
 * its label list at xmbThemeNames; drawXMBBackground() follows xmbThemeIndex
 * automatically, so switching the theme updates the background live.
 */
#define XMB_THEME_COUNT 13

#ifdef __cplusplus
extern "C" {
#endif

extern const char *const xmbThemeNames[XMB_THEME_COUNT];
extern uint8_t           xmbThemeIndex; // 0 .. XMB_THEME_COUNT - 1

/*
 * Colour palettes for the PSP-derived wave themes. Selecting one recolours
 * the background gradient, both wave layers and the sparkles together, so the
 * whole scene stays harmonised rather than just tinting one element.
 */
#define XMB_PALETTE_COUNT 20

extern const char *const xmbPaletteNames[XMB_PALETTE_COUNT];
extern uint8_t           xmbPaletteIndex; // 0 .. XMB_PALETTE_COUNT - 1

// Index of the theme whose customization submenu the menu should offer.
// This is "Default" (formerly "Gouraud Waves + Sparkle + PSP"), moved to
// index 0 when it became the first/default theme.
#define XMB_PALETTE_THEME_INDEX 0

/*
 * Icon shading style for the XMB category ribbon. Default draws the icons as
 * flat white (modulated normally); the two gradient styles tint them with a
 * light or dark vertical gradient derived automatically from the current
 * colour palette, so they read against whatever background is active without
 * merging into it.
 */
#define XMB_ICON_STYLE_COUNT 3
extern const char *const xmbIconStyleNames[XMB_ICON_STYLE_COUNT];
extern uint8_t           xmbIconStyle;   // 0 Default, 1 Light, 2 Dark

// Fill *top/*bot with the GP0 modulation colours for the current icon style
// and palette. Only meaningful when xmbIconStyle != 0 (Default = plain white).
void xmbGetIconGradient(uint32_t *top, uint32_t *bot);

/*
 * Dominant colour of the current theme, for screens that draw their own
 * translucent "crystal" tiles (the memory card grid, the CD player track
 * list) and want to follow the wallpaper rather than stay permanently blue.
 *
 * *base is a dark tint meant to be drawn blended and then lightened by the
 * caller for its sheen and bevel; *glow is a bright, near-white version for a
 * selection bloom. Either pointer may be NULL.
 */
void xmbGetAccentColor(uint32_t *base, uint32_t *glow);

/*
 * Wave style for the customizable PSP wave theme: 10 variants ranging from a
 * few fat slow ribbons to many thin fast ones. Named "Style 1".."Style 10"
 * in the menu; drawGouraudPSP() reads xmbWaveStyle to pick band count and
 * thickness.
 */
#define XMB_WAVE_STYLE_COUNT 10
extern uint8_t xmbWaveStyle;   // 0 .. XMB_WAVE_STYLE_COUNT - 1

// Pick a style directly (bypasses the theme index). Normally you just set
// xmbThemeIndex via the menu instead. Defaults to XMB_BG_GOURAUD_WAVES.
void xmbBgSetStyle(XMBBgStyle style);
XMBBgStyle xmbBgGetStyle(void);

// Draw the background into the current frame's DMA chain. Advances its own
// animation phase only while isBackgroundScrollEnabled() is true.
// A cheap flat-colour alternative to the live theme, for screens where the
// full theme's primitive cost is a real risk (see ramtester.c/ramconfig.c
// and ui.c's isHeavyBackgroundUnsafe()).
void drawFlatBackdrop(RenderContext *ctx, uint32_t color);

void drawXMBBackground(RenderContext *ctx);

// Uploads the Nebula 2 hero planet's surface texture into VRAM. Call once at
// boot, alongside initIcons() - see xmb_bg.c for the VRAM placement.
void initNebulaTexture(RenderContext *ctx);

#ifdef __cplusplus
}
#endif
