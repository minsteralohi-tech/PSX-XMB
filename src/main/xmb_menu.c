/*
 * PSX-iTests - PSP/PS3-style XMB cross menu (see xmb_menu.h)
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "common/gpu.h"
#include "common/sio0.h"
#include "main/badge.h"
#include "main/launch_ui.h"
#include "main/music_settings.h"
#include "main/cd_player.h"
#include "main/console_info.h"
#include "main/cpu_bench.h"
#include "main/fastboot.h"
#include "main/unirom_launch.h"
#include "main/defs.h"
#include "main/font.h"
#include "main/gpu_colorbars.h"
#include "main/gpu_cube.h"
#include "main/icon.h"
#include "main/mainmenu.h"
#include "main/memcard.h"
#include "main/model_test.h"
#include "main/modals.h"
#include "main/pad_test.h"
#include "main/ramtester.h"
#include "main/renderer.h"
#include "main/sound.h"
#include "main/spu_channel_test.h"
#include "main/ui.h"
#include "main/xmb_bg.h"
#include "main/xmb_menu.h"
#include "ps1/gpucmd.h"

/* --- data model --------------------------------------------------------- */

typedef struct {
	const char   *name;
	int           icon;
	MenuCallback  action;
	bool          direct;  // true: self-contained screen, XMB resumes right
	                        // after it returns. false: hands off to a classic
	                        // list submenu, which re-activates the XMB itself
	                        // (via enterMainMenu) whenever the user backs out.
} XMBEntry;

typedef struct {
	const char     *name;
	int             icon;      // index into the CATEGORY icon sheet
	const XMBEntry *items;
	int             itemCount;
	MenuCallback    direct;   // for categories that launch immediately
	bool            isThemes; // special: items are the background theme list
} XMBCategory;

/* Forward-declared local actions. */
static void xmbFastBoot(RenderContext *ctx, UIState *state, const MenuItem *item);
static void xmbLaunchUniROM(RenderContext *ctx, UIState *state, const MenuItem *item);
static void xmbReturnToUniROMCart(RenderContext *ctx, UIState *state, const MenuItem *item);

/* Icon indices refer to the 12-slot textured item sheet (assets/icons.png).
 * The CATEGORY icons are not in this sheet - they're drawn as vector
 * sheet (assets/icons_cat.png) - see icon.c for the layout. */
static const XMBEntry settingsItems[] = {
	{ "Console Information", 0, runConsoleInfo,     true  },
	{ "Trophy",              11, runTrophyRoom,      true  },
	{ "SIO Loader",          0,  runSIOLoader,       true  },
	{ "Music",               0,  runMusicSettings,   true  },
	{ "Reboot",              1, doFullReboot,       false },
	{ "About",               2, enterAboutMenu,     false },
};
// Index of "Music" within settingsItems[] above - selecting it opens the
// BGM/SFX flyout (see musicMenuOpen) instead of dispatching its own
// .action callback (runMusicSettings is kept only as a documented no-op
// fallback - see its own header comment - since ITEM_ACTION dispatch
// requires every entry to have a non-null callback).
#define MUSIC_ITEM_INDEX 3

// True if `cat`/`itemIndex` is exactly the "Music" row within Settings -
// checked by comparing the category's own item array pointer against
// settingsItems[], the same style XMB_PALETTE_THEME_INDEX already uses to
// identify one specific row.
static bool isMusicItem(const XMBCategory *cat, int itemIndex) {
	return (cat->items == settingsItems) && (itemIndex == MUSIC_ITEM_INDEX);
}
static const XMBEntry gameItems[] = {
	{ "Normal Boot", 3, doNormalBoot,        false },
	{ "Fast Boot",   4, xmbFastBoot,         false },
};
static const XMBEntry hwItems[] = {
	{ "GPU Color Bar",      5, runColorBarTest,    true },
	{ "GPU Spinning Cube",  6, runGPUCubeTest,     true },
	{ "Test PlayStation Model", 5, runModelTest,   true },
	{ "CPU Benchmark",      7, runCPUBenchmark,    true },
	{ "SPU Channel Test",   8, runSPUChannelTest,  true },
	{ "Pad Tester",         9, runPadTest,         true },
	{ "PS1 RAM Tester",    10, enterRAMTesterMenu, false },
	{ "UniROM 8.0",         8, xmbLaunchUniROM,    true },
	{ "UniROM (cart installed)", 8, xmbReturnToUniROMCart, true },
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* Themes is now a top-level category of its own: its item list is the
 * background theme list (xmbThemeNames), generated at runtime rather than
 * stored here, so adding a theme in xmb_bg.c automatically shows up.
 *
 * The former "Tools" category held BIOS Memory Dump, CD-ROM Test, Main RAM
 * Size Test and GPU Version Test. The dump was dropped and the other three
 * were folded into the Console Information page, leaving it empty, so it is
 * no longer listed. */
static const XMBCategory categories[] = {
	{ "Settings",            0, settingsItems, COUNT(settingsItems), 0, false },
	{ "Themes",              1, 0, 0, 0, true  },
	{ "Music",               2, 0, 0, runCDPlayer,          false },
	{ "Game",                3, gameItems, COUNT(gameItems), 0, false },
	{ "Memory Card Manager", 4, 0, 0, runMemoryCardManager, false },
	{ "Hardware Tester",     5, hwItems, COUNT(hwItems), 0, false },
};
#define NUM_CATEGORIES COUNT(categories)

// How many selectable rows the given category shows.
static int categoryItemCount(const XMBCategory *cat) {
	return cat->isThemes ? XMB_THEME_COUNT : cat->itemCount;
}

/* --- state -------------------------------------------------------------- */

static bool active    = false;
static int  catIndex  = 0;
static int  itemIndex = 0;
static int  catPosFx  = 0;   // fixed point (*256) for smooth gliding
static int  itemPosFx = 0;

/*
 * Palette submenu. The PSP wave theme supports recolouring, so selecting it
 * in the Themes list opens a second level listing the palettes instead of
 * immediately closing. Circle backs out to the theme list.
 */
/*
 * Theme customization menu. Selecting the recolourable PSP wave theme opens a
 * PS3-style two-level menu: a left "options" column (Icons / Color / Wave
 * Style / Music) and, when an option is chosen, a value column that slides in
 * from the right. Circle backs out one level at a time.
 */
static bool themeMenuOpen = false;   // options column open
static int  themeOptIndex = 0;       // 0 .. THEME_OPT_COUNT - 1
static bool subMenuOpen   = false;   // right value column open
static int  subIndex      = 0;       // highlighted value in the right column
static int  subPosFx      = 0;       // glide (*256) for the value column
static int  slideFx       = 0;       // 0..256 slide-in of the right column

/*
 * Music settings flyout. Same two-level pattern as the theme customization
 * menu just above (a left options column - BGM / SFX - and a value column
 * that slides in from the right), triggered by selecting "Music" under
 * Settings instead of that item's own callback taking over the whole
 * screen - the XMB (category ribbon, item list) stays visible underneath,
 * exactly like the theme flyout does. Kept as a fully separate set of state
 * variables rather than reusing the theme ones so the two flyouts can never
 * interfere with each other.
 */
static bool musicMenuOpen    = false;   // options column open
static int  musicOptIndex    = 0;       // 0 = BGM, 1 = SFX
static bool musicSubMenuOpen = false;   // right value column open
static int  musicSubIndex    = 0;       // highlighted value in the right column
static int  musicSubPosFx    = 0;       // glide (*256) for the value column
static int  musicSlideFx     = 0;       // 0..256 slide-in of the right column

void initXMB(void) {
	active = false; catIndex = 0; itemIndex = 0; catPosFx = 0; itemPosFx = 0;
	themeMenuOpen = false; themeOptIndex = 0;
	subMenuOpen = false; subIndex = 0; subPosFx = 0; slideFx = 0;
	musicMenuOpen = false; musicOptIndex = 0;
	musicSubMenuOpen = false; musicSubIndex = 0; musicSubPosFx = 0; musicSlideFx = 0;
}
bool isXMBActive(void) { return active; }
void setXMBActive(bool a) { active = a; }

/* --- local actions ------------------------------------------------------ */

static void xmbFastBoot(RenderContext *ctx, UIState *state, const MenuItem *item) {
	(void) ctx; (void) state; (void) item;
	launchLoader();
}

/* UniROM 8.0: chain-load the embedded real UniROM build (see
 * unirom_launch.c). Never returns. */
static void xmbLaunchUniROM(RenderContext *ctx, UIState *state, const MenuItem *item) {
	// The confirmation screen, the plan validation and the handoff all live
	// in launch_ui.c now, shared with Settings -> SIO Loader, so the two
	// cannot drift apart. Returns if the user backs out.
	runUniROMLauncher(ctx, state, item);
}

/* For consoles that boot into this app THROUGH a real, physically-installed
 * UniROM cartridge: use this instead of "UniROM 8.0" above. That option
 * copies a second, independent UniROM image into RAM, which fights the
 * cart's own resident firmware (that's what was producing the RAM error -
 * see unirom_launch.c). This just hands control back the way a real
 * soft-reset would; the cart intercepts that at the hardware level and its
 * own menu comes back. */
static void xmbReturnToUniROMCart(RenderContext *ctx, UIState *state, const MenuItem *item) {
	(void) ctx; (void) state; (void) item;
	returnToUniROMCart();
}

/* --- drawing helpers ---------------------------------------------------- */

#define TEXT_WHITE  0x808080
#define TEXT_DIM    0x404040

/* Draw a small filled circle (~9 px) as a stack of flat mono quads - used as
 * the "currently active" marker in the theme list and the value columns, in
 * place of the old "." which was too small to notice. */
static void drawDisc(GPUDMAChain *chain, int cx, int cy, uint32_t color) {
	static const int hw[9] = { 2, 3, 4, 4, 4, 4, 4, 3, 2 };
	for (int i = 0; i < 9; i++) {
		int dy = i - 4;
		int w  = hw[i];
		uint32_t *p = allocateGP0Packet(chain, 5);
		p[0] = color | gp0_shadedQuad(false, false, false);
		p[1] = gp0_xy(cx - w, cy + dy);          // TL
		p[2] = gp0_xy(cx + w, cy + dy);          // TR
		p[3] = gp0_xy(cx - w, cy + dy + 1);      // BL
		p[4] = gp0_xy(cx + w, cy + dy + 1);      // BR
	}
}

/* --- theme customization options ---------------------------------------- */

// Music used to be a 4th option here; it's now its own entry under Settings
// (see runMusicSettings() / settingsItems below) since BGM selection isn't
// really specific to this one theme's customization, and more tracks are
// coming later.
enum { OPT_ICONS = 0, OPT_COLOR, OPT_WAVE, THEME_OPT_COUNT };

static const char *const themeOptNames[THEME_OPT_COUNT] = {
	"Icons", "Color", "Wave Style"
};

static int themeOptValueCount(int opt) {
	switch (opt) {
	case OPT_ICONS: return XMB_ICON_STYLE_COUNT;
	case OPT_COLOR: return XMB_PALETTE_COUNT;
	case OPT_WAVE:  return XMB_WAVE_STYLE_COUNT;
	}
	return 0;
}

// The value index currently applied for this option (drives the marker and
// the initial highlight when the value column opens).
static int themeOptCurrent(int opt) {
	switch (opt) {
	case OPT_ICONS: return xmbIconStyle;
	case OPT_COLOR: return xmbPaletteIndex;
	case OPT_WAVE:  return xmbWaveStyle;
	}
	return 0;
}

static const char *themeOptValueName(int opt, int i) {
	static char waveBuf[12];
	switch (opt) {
	case OPT_ICONS: return xmbIconStyleNames[i];
	case OPT_COLOR: return xmbPaletteNames[i];
	case OPT_WAVE:  snprintf(waveBuf, sizeof(waveBuf), "Style %d", i + 1);
	                return waveBuf;
	}
	return "";
}

static void themeOptApply(int opt, int i) {
	switch (opt) {
	case OPT_ICONS: xmbIconStyle    = (uint8_t) i; break;
	case OPT_COLOR: xmbPaletteIndex = (uint8_t) i; break;
	case OPT_WAVE:  xmbWaveStyle    = (uint8_t) i; break;
	}
}

/* --- layout ------------------------------------------------------------- */

#define ROW_Y      58   // category icon centre line
#define SEL_X      86   // selected category screen x
#define CAT_GAP    52   // spacing between categories
#define ITEM_X     44   // item icon centre x
#define ITEM_TOP   104  // first item y
#define ITEM_GAP   26

/* rough centred-ish text: ~6 px per glyph */
static int textLeftFor(const char *s, int centreX) {
	int n = 0;
	while (s[n]) n++;
	return centreX - (n * 6) / 2;
}

void renderXMB(RenderContext *ctx, UIState *state) {
	(void) state;

	drawXMBBackground(ctx);

	// Glide toward targets. The category ribbon and the theme item list use
	// catPosFx / itemPosFx; the theme customization value column has its own
	// glide (subPosFx) and a 0..256 slide-in (slideFx) so it eases in from the
	// right edge instead of popping into place.
	catPosFx  += ((catIndex  * 256) - catPosFx)  >> 2;
	itemPosFx += ((itemIndex * 256) - itemPosFx) >> 2;
	subPosFx  += ((subIndex  * 256) - subPosFx)  >> 2;

	int slideTarget = subMenuOpen ? 256 : 0;
	slideFx += (slideTarget - slideFx) >> 2;
	if (slideFx < 0)   slideFx = 0;
	if (slideFx > 256) slideFx = 256;

	musicSubPosFx += ((musicSubIndex * 256) - musicSubPosFx) >> 2;
	int musicSlideTarget = musicSubMenuOpen ? 256 : 0;
	musicSlideFx += (musicSlideTarget - musicSlideFx) >> 2;
	if (musicSlideFx < 0)   musicSlideFx = 0;
	if (musicSlideFx > 256) musicSlideFx = 256;

	// header - "PSX-XMB", smaller than the old "PSX-iTests" and in the same
	// dim colour as the Category/Item/Select footer hint below, rather than
	// full white.
	printString(ctx, 12, 10, TEXT_DIM, "PSX-XMB");

	// --- category ribbon ---
	for (int i = 0; i < NUM_CATEGORIES; i++) {
		int dxFx = i * 256 - catPosFx;
		int x    = SEL_X + (dxFx * CAT_GAP) / 256;
		if (x < -40 || x > 360)
			continue;

		int adx = dxFx < 0 ? -dxFx : dxFx;
		bool sel = adx < 128;
		int size = sel ? 32 : 22;

		// Selected category is fully opaque; the rest are drawn with the
		// PS1's semi-transparency blend so they read as faded against the
		// animated background, leaving the selected one as the focal point.
		//
		// In the Gouraud Waves theme the row icons pick up a light-blue
		// vertical gradient (icy blue at the top fading to the theme's deeper
		// blue at the bottom) sampled from the wave palette, so they read as
		// part of the blue theme and stand out against the animated wash -
		// done with a POLY_GT4 (Gouraud + textured) quad, no extra artwork.
		// Icon shading follows the theme's "Icons" customization:
		//   Default        - flat white, modulated normally
		//   Light/Dark     - a vertical gradient derived automatically from
		//                     the current colour palette (see xmbGetIconGradient),
		//                     so the icons tint to match whatever background is
		//                     active without merging into it.
		// The original Gouraud Waves theme keeps its fixed icy-blue gradient.
		if (xmbIconStyle != 0) {
			uint32_t top, bot;
			xmbGetIconGradient(&top, &bot);
			drawCategoryIconGradient(ctx, categories[i].icon,
				x - size / 2, ROW_Y - size / 2, size, !sel, top, bot);
		} else if (xmbBgGetStyle() == XMB_BG_GOURAUD_WAVES) {
			drawCategoryIconGradient(ctx, categories[i].icon,
				x - size / 2, ROW_Y - size / 2, size, !sel,
				gp0_rgb(0x50, 0x64, 0x80),   // top: light icy blue
				gp0_rgb(0x24, 0x40, 0x78));  // bottom: deeper theme blue
		} else {
			drawCategoryIcon(ctx, categories[i].icon, x - size / 2,
				ROW_Y - size / 2, size, !sel);
		}

		// Category name only appears under the currently selected icon.
		if (sel)
			printString(ctx, textLeftFor(categories[i].name, x),
				ROW_Y + 22, TEXT_WHITE, categories[i].name);
	}

	// --- items of the selected category ---
	const XMBCategory *cat = &categories[catIndex];
	int count = categoryItemCount(cat);

	// Theme customization menu takes over the item area when open: a left
	// "options" column (Icons / Color / Wave Style / Music) plus, when an
	// option is chosen, a value column that slides in from the right.
	if (cat->isThemes && themeMenuOpen) {
		#define OPT_X (ITEM_X + 8)   // options column, indented from theme list

		// Title sits at y=90, clear of the category name above (ROW_Y+22=80).
		printString(ctx, OPT_X, ITEM_TOP - 14, TEXT_WHITE, "Customize");

		for (int j = 0; j < THEME_OPT_COUNT; j++) {
			int y = ITEM_TOP + j * ITEM_GAP;
			bool sel = (j == themeOptIndex);
			printString(ctx, OPT_X, y - 4,
				sel ? TEXT_WHITE : TEXT_DIM, themeOptNames[j]);
			// A ">" cue on the focused option points to the value column.
			if (sel)
				printString(ctx, OPT_X - 10, y - 4, TEXT_WHITE, ">");
		}

		// Value column: present while open, and during the slide-out anim.
		if (subMenuOpen || slideFx > 0) {
			int restX = 168;
			// slideFx 0 -> off right edge (x=320), 256 -> resting (x=restX).
			int x = restX + ((320 - restX) * (256 - slideFx)) / 256;

			int cnt     = themeOptValueCount(themeOptIndex);
			int applied = themeOptCurrent(themeOptIndex);

			printString(ctx, x, ITEM_TOP - 14, TEXT_WHITE,
				themeOptNames[themeOptIndex]);

			for (int j = 0; j < cnt; j++) {
				int dyFx = j * 256 - subPosFx;
				int y = ITEM_TOP + (dyFx * ITEM_GAP) / 256;
				if (y < ITEM_TOP - 12 || y > 224)
					continue;

				int ady = dyFx < 0 ? -dyFx : dyFx;
				bool sel = subMenuOpen && ady < 128;

				printString(ctx, x + 14, y - 4,
					sel ? TEXT_WHITE : TEXT_DIM,
					themeOptValueName(themeOptIndex, j));
				// Circle marks the value that's currently applied.
				if (j == applied)
					drawDisc(getCurrentChain(ctx), x + 6, y - 1, TEXT_WHITE);
			}
		}

		printString(ctx, 12, 214, TEXT_DIM,
			CH_PS1_DPAD_Y " Move   "
			CH_PS1_CROSS_BUTTON " Select   "
			CH_PS1_CIRCLE_BUTTON " Back");
		return;
	}

	// Music settings flyout - same layout as the theme customization menu
	// just above, just with "BGM"/"SFX" as the options column and each
	// one's own track/set list as the value column.
	if (isMusicItem(cat, itemIndex) && musicMenuOpen) {
		#define MUSIC_OPT_X (ITEM_X + 8)

		printString(ctx, MUSIC_OPT_X, ITEM_TOP - 14, TEXT_WHITE, "Music");

		static const char *const musicOptNames[2] = { "BGM", "SFX" };
		for (int j = 0; j < 2; j++) {
			int y = ITEM_TOP + j * ITEM_GAP;
			bool sel = (j == musicOptIndex);
			printString(ctx, MUSIC_OPT_X, y - 4,
				sel ? TEXT_WHITE : TEXT_DIM, musicOptNames[j]);
			if (sel)
				printString(ctx, MUSIC_OPT_X - 10, y - 4, TEXT_WHITE, ">");
		}

		if (musicSubMenuOpen || musicSlideFx > 0) {
			int restX = 168;
			int x = restX + ((320 - restX) * (256 - musicSlideFx)) / 256;

			bool isBGM  = (musicOptIndex == 0);
			int cnt     = isBGM ? getBGMCount()   : getSFXSetCount();
			int applied = isBGM ? getBGMIndex()   : getSFXSetIndex();

			printString(ctx, x, ITEM_TOP - 14, TEXT_WHITE, musicOptNames[musicOptIndex]);

			for (int j = 0; j < cnt; j++) {
				int dyFx = j * 256 - musicSubPosFx;
				int y = ITEM_TOP + (dyFx * ITEM_GAP) / 256;
				if (y < ITEM_TOP - 12 || y > 224)
					continue;

				int ady = dyFx < 0 ? -dyFx : dyFx;
				bool sel = musicSubMenuOpen && ady < 128;

				printString(ctx, x + 14, y - 4,
					sel ? TEXT_WHITE : TEXT_DIM,
					isBGM ? getBGMName(j) : getSFXSetName(j));
				if (j == applied)
					drawDisc(getCurrentChain(ctx), x + 6, y - 1, TEXT_WHITE);
			}
		}

		printString(ctx, 12, 214, TEXT_DIM,
			CH_PS1_DPAD_Y " Move   "
			CH_PS1_CROSS_BUTTON " Select   "
			CH_PS1_CIRCLE_BUTTON " Back");
		return;
	}

	for (int j = 0; j < count; j++) {
		int dyFx = j * 256 - itemPosFx;
		int y = ITEM_TOP + (dyFx * ITEM_GAP) / 256;
		if (y < ITEM_TOP - 24 || y > 226)
			continue;

		int ady = dyFx < 0 ? -dyFx : dyFx;
		bool sel = ady < 128;

		// The selected row is distinguished purely by being bigger,
		// brighter and fully opaque - no highlight bar, no drop shadow.
		if (cat->isThemes) {
			// Theme rows are text-only, with a marker on the active theme.
			const char *nm = xmbThemeNames[j];
			printString(ctx, ITEM_X, y - 4, sel ? TEXT_WHITE : TEXT_DIM, nm);
			// Circle marks the currently active theme (was a tiny ".").
			if (j == xmbThemeIndex)
				drawDisc(getCurrentChain(ctx), ITEM_X - 7, y - 1, TEXT_WHITE);
		} else {
			int isz = sel ? 18 : 14;
			drawIcon(ctx, cat->items[j].icon, ITEM_X - isz / 2, y - isz / 2, isz, !sel);
			printString(ctx, ITEM_X + 14, y - 4,
				sel ? TEXT_WHITE : TEXT_DIM, cat->items[j].name);
		}
	}

	// footer hint
	printString(ctx, 12, 214, TEXT_DIM,
		CH_PS1_DPAD_X " Category   "
		CH_PS1_DPAD_Y " Item   "
		CH_PS1_CROSS_BUTTON " Select");
}

void updateXMB(RenderContext *ctx, UIState *state, uint16_t buttons) {
	updateUIState(state, buttons);

	uint16_t nav = state->buttonsPressed | state->buttonsRepeating;

	// Check BEFORE mutating catIndex: while the theme customize menu OR the
	// Music flyout is open, Left/Right must not fall through to the
	// category-ribbon switch below (see the longer comment further down) -
	// pressing Left there backs out one level instead.
	bool inThemeCustomize = categories[catIndex].isThemes && themeMenuOpen;
	bool inMusicFlyout    = isMusicItem(&categories[catIndex], itemIndex) && musicMenuOpen;

	// Navigation stops at the ends instead of wrapping around, both
	// horizontally (category row) and vertically (item list).
	if (!inThemeCustomize && !inMusicFlyout) {
		if (nav & PAD_BTN_LEFT) {
			if (catIndex > 0) {
				themeMenuOpen = false; subMenuOpen = false;
				musicMenuOpen = false; musicSubMenuOpen = false;
				catIndex--;
				itemIndex = 0; itemPosFx = 0;
				playScrollSound();
			}
		}
		if (nav & PAD_BTN_RIGHT) {
			if (catIndex < NUM_CATEGORIES - 1) {
				themeMenuOpen = false; subMenuOpen = false;
				musicMenuOpen = false; musicSubMenuOpen = false;
				catIndex++;
				itemIndex = 0; itemPosFx = 0;
				playScrollSound();
			}
		}
	}

	const XMBCategory *cat = &categories[catIndex];
	int count = categoryItemCount(cat);

	// While the theme customization menu is open it owns ALL input,
	// including Left/Right - those must NOT fall through to the category
	// ribbon switch below, or pressing Left here would jump to a different
	// top-level category instead of backing out one level (the reported
	// bug). Left acts as Back at this level, exactly like Circle; only once
	// we're back at the plain theme list does Left/Right resume switching
	// categories, same as a modern PSP-style menu.
	if (cat->isThemes && themeMenuOpen) {
		// Right value column has focus: navigate/apply values, Circle/Left
		// backs out to the options column.
		if (subMenuOpen) {
			int cnt = themeOptValueCount(themeOptIndex);
			if (nav & PAD_BTN_UP) {
				if (subIndex > 0) { subIndex--; playScrollSound(); }
			}
			if (nav & PAD_BTN_DOWN) {
				if (subIndex < cnt - 1) { subIndex++; playScrollSound(); }
			}
			if ((state->buttonsPressed & PAD_BTN_CROSS) || (nav & PAD_BTN_RIGHT)) {
				themeOptApply(themeOptIndex, subIndex);
				playConfirmSound();
			}
			if ((state->buttonsPressed & PAD_BTN_CIRCLE) || (nav & PAD_BTN_LEFT)) {
				subMenuOpen = false;   // slide the column back out
				playCancelSound();
			}
			return;
		}

		// Options column has focus: pick an option (opens its value column),
		// Circle/Left backs out to the theme list.
		if (nav & PAD_BTN_UP) {
			if (themeOptIndex > 0) { themeOptIndex--; playScrollSound(); }
		}
		if (nav & PAD_BTN_DOWN) {
			if (themeOptIndex < THEME_OPT_COUNT - 1) {
				themeOptIndex++; playScrollSound();
			}
		}
		if ((state->buttonsPressed & PAD_BTN_CROSS) || (nav & PAD_BTN_RIGHT)) {
			subMenuOpen = true;
			subIndex    = themeOptCurrent(themeOptIndex);
			subPosFx    = subIndex * 256;
			playConfirmSound();
		}
		if ((state->buttonsPressed & PAD_BTN_CIRCLE) || (nav & PAD_BTN_LEFT)) {
			themeMenuOpen = false;
			itemIndex = XMB_PALETTE_THEME_INDEX;
			itemPosFx = itemIndex * 256;
			playCancelSound();
		}
		return;
	}

	// Music settings flyout - same structure as the theme customize input
	// above. Deliberately does NOT live-preview on Up/Down (an earlier
	// version of this screen did, auto-applying every track/set as you
	// scrolled past it) - selecting is only ever applied on Cross, exactly
	// matching how the theme value column itself already behaves, which is
	// what was actually asked for here.
	if (isMusicItem(cat, itemIndex) && musicMenuOpen) {
		if (musicSubMenuOpen) {
			bool isBGM = (musicOptIndex == 0);
			int cnt = isBGM ? getBGMCount() : getSFXSetCount();

			if (nav & PAD_BTN_UP) {
				if (musicSubIndex > 0) { musicSubIndex--; playScrollSound(); }
			}
			if (nav & PAD_BTN_DOWN) {
				if (musicSubIndex < cnt - 1) { musicSubIndex++; playScrollSound(); }
			}
			if ((state->buttonsPressed & PAD_BTN_CROSS) || (nav & PAD_BTN_RIGHT)) {
				if (isBGM)
					selectBGM(musicSubIndex);
				else
					selectSFXSet(musicSubIndex);
				playConfirmSound();
			}
			if ((state->buttonsPressed & PAD_BTN_CIRCLE) || (nav & PAD_BTN_LEFT)) {
				musicSubMenuOpen = false;   // slide the column back out
				playCancelSound();
			}
			return;
		}

		// Options column has focus: pick BGM or SFX (opens its value
		// column), Circle/Left backs out of the whole flyout.
		if (nav & PAD_BTN_UP) {
			if (musicOptIndex > 0) { musicOptIndex--; playScrollSound(); }
		}
		if (nav & PAD_BTN_DOWN) {
			if (musicOptIndex < 1) { musicOptIndex++; playScrollSound(); }
		}
		if ((state->buttonsPressed & PAD_BTN_CROSS) || (nav & PAD_BTN_RIGHT)) {
			musicSubMenuOpen = true;
			musicSubIndex    = (musicOptIndex == 0) ? getBGMIndex() : getSFXSetIndex();
			musicSubPosFx    = musicSubIndex * 256;
			playConfirmSound();
		}
		if ((state->buttonsPressed & PAD_BTN_CIRCLE) || (nav & PAD_BTN_LEFT)) {
			musicMenuOpen = false;
			itemIndex = MUSIC_ITEM_INDEX;
			itemPosFx = itemIndex * 256;
			playCancelSound();
		}
		return;
	}

	if (count > 0) {
		if (nav & PAD_BTN_UP) {
			if (itemIndex > 0) {
				itemIndex--;
				playScrollSound();
			}
		}
		if (nav & PAD_BTN_DOWN) {
			if (itemIndex < count - 1) {
				itemIndex++;
				playScrollSound();
			}
		}
	}

	if (state->buttonsPressed & PAD_BTN_CROSS) {
		// Themes: selecting a row switches the background theme and stays
		// on the XMB, so the change is visible immediately behind the menu.
		if (cat->isThemes) {
			xmbThemeIndex = (uint8_t) itemIndex;
			playConfirmSound();

			// This theme is customizable, so selecting it opens its
			// customization menu (Icons / Color / Wave Style / Music) rather
			// than just applying and stopping.
			if (itemIndex == XMB_PALETTE_THEME_INDEX) {
				themeMenuOpen = true;
				themeOptIndex = 0;
				subMenuOpen   = false;
				slideFx       = 0;
			}
			return;
		}

		// Music: opens the BGM/SFX flyout in place, same idea as the theme
		// customize menu above - the XMB stays visible underneath rather
		// than navigating away to a separate full-screen settings page.
		if (isMusicItem(cat, itemIndex)) {
			playConfirmSound();
			musicMenuOpen    = true;
			musicOptIndex    = 0;
			musicSubMenuOpen = false;
			musicSlideFx     = 0;
			return;
		}

		bool isDirect;
		MenuCallback cb;

		if (cat->itemCount > 0) {
			cb       = cat->items[itemIndex].action;
			isDirect = cat->items[itemIndex].direct;
		} else {
			// Both direct-launch categories (Music, Memory Card Manager)
			// are self-contained screens with their own loop.
			cb       = cat->direct;
			isDirect = true;
		}

		if (cb) {
			playConfirmSound();

			// Hand off to the callback. Self-contained screens ("direct")
			// run their own loop and simply return when the user backs out,
			// without touching state->currentMenu - so we must re-activate
			// the XMB ourselves right after. Callbacks that instead switch
			// to a classic list submenu (About, Reboot confirm, RAM Tester,
			// Normal Boot confirm) need the XMB OFF so that submenu's own
			// list UI shows; enterMainMenu() re-activates the XMB whenever
			// the user eventually backs out of that submenu.
			active = false;
			cb(ctx, state, 0);
			if (isDirect)
				active = true;
		}
	}
}
