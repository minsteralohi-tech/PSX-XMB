/*
 * PSX-iTests - live system status HUD (see hud.h for what the numbers mean)
 */

#include <stdint.h>
#include <stdio.h>
#include "common/gpu.h"
#include "common/spu.h"
#include "main/console_info.h"
#include "main/font.h"
#include "main/hud.h"
#include "main/ramtester.h"
#include "main/renderer.h"
#include "main/sound.h"
#include "ps1/gpucmd.h"

// Linker symbol marking the end of this program's code+data+bss - the same
// boundary ramtester.c already treats as "everything above this is free
// RAM, safe to test". Used here as the "RAM used" figure.
extern char _bssEnd[];

#define RAM_BASE_ADDR 0x80010000u   // matches executable.ld's APP_RAM origin

// Fixed VRAM layout constants, mirrored from renderer.c / icon.c (all in
// halfword units - see icon.c's "VRAM placement" comment for the full map).
#define VRAM_BG_FONT_BYTES   (64  * 112 * 2)   // x 640..704, y 0..112
#define VRAM_ICON_BYTES      (128 * 272 * 2)   // x 704..832, y 0..272 (incl. CLUTs)
// Sun/lava/earth/moon planet textures (xmb_bg.c's initNebulaTexture()),
// stacked at the same X column: 128 VRAM columns (256 texels at 8bpp) wide,
// 512 rows tall (4 textures x 128 rows each) - this was missing before,
// which mattered more than it looks: it's not just an undercount, it's
// exactly the block of VRAM that was going stale/wrong before the
// reloadTextures() fix (see that function's comment) - so while that bug
// existed, this figure was quietly wrong every time it mattered most.
#define VRAM_NEBULA_BYTES    (128 * 512 * 2)

static uint32_t detectedRAMMB    = 2;
static uint32_t usedRAMBytes     = 0;
static uint32_t vramUsedBytes    = 0;
static uint32_t vramCapacityBytes = 0x100000;
static bool     hudEnabled       = false;   // off by default

void initSystemHUD(void) {
	detectedRAMMB = detectPhysicalRAMSizeMB();

	usedRAMBytes = (uint32_t) _bssEnd - RAM_BASE_ADDR;

	// Framebuffers depend on the actual screen resolution (320x240 for both
	// NTSC and PAL here), computed the same way renderer.c's reloadTextures()
	// derives its texture start X (screenWidth * 2 halfwords wide, i.e. two
	// side-by-side framebuffers).
	uint32_t fbBytes = (uint32_t) (320 * 2) * 240 * 2;
	vramUsedBytes = fbBytes + VRAM_BG_FONT_BYTES + VRAM_ICON_BYTES + VRAM_NEBULA_BYTES;

	vramCapacityBytes = (1u << vramSize) * 0x100000u;
}

uint32_t getDetectedRAMMB(void) {
	return detectedRAMMB;
}

bool toggleSystemHUD(void) {
	hudEnabled = !hudEnabled;
	return hudEnabled;
}

bool isSystemHUDEnabled(void) {
	return hudEnabled;
}

bool isBadge8MBRAM(void)  { return detectedRAMMB >= 8; }
bool isBadge16MBRAM(void) { return detectedRAMMB >= 16; }
bool isBadge2MBVRAM(void) { return vramSize >= 1; }
bool isBadgeSPUVerified(void) { return isSPURAMTestPassed(); }

int getUnlockedBadgeCount(void) {
	int n = 0;
	if (isBadge8MBRAM())     n++;
	if (isBadge16MBRAM())    n++;
	if (isBadge2MBVRAM())    n++;
	if (isBadgeSPUVerified()) n++;
	return n;
}

bool isFeature1Unlocked(void) { return getUnlockedBadgeCount() >= 1; }
bool isFeature2Unlocked(void) { return getUnlockedBadgeCount() >= 3; }

/* --- drawing -------------------------------------------------------------*/

// Small filled rectangle - same 3-word pattern used throughout xmb_bg.c's
// primitive helpers (color+cmd, xy, wh).
static void hudRect(
	GPUDMAChain *chain, int x, int y, int w, int h, uint32_t color
) {
	uint32_t *ptr = allocateGP0Packet(chain, 3);
	ptr[0] = color | gp0_rectangle(false, false, false);
	ptr[1] = gp0_xy(x, y);
	ptr[2] = gp0_xy(w, h);
}

#define BAR_W  24
#define BAR_H   4

// One "LABEL: [bar] used / total MB" row.
static void hudMeterRow(
	RenderContext *ctx, int x, int y, const char *label,
	uint32_t usedBytes, uint32_t totalBytes
) {
	GPUDMAChain *chain = getCurrentChain(ctx);

	printString(ctx, x, y, 0x606060, label);
	int barX = x + 26;

	// Track (empty bar background) + border tick.
	hudRect(chain, barX, y + 1, BAR_W, BAR_H, gp0_rgb(30, 30, 34));

	int filled = totalBytes ? (int) (((uint64_t) usedBytes * BAR_W) / totalBytes) : 0;
	if (filled > BAR_W) filled = BAR_W;
	if (filled < 0) filled = 0;

	// Colour reads green when there's headroom, amber as it fills up, so the
	// "more RAM = more headroom" point comes across without reading numbers.
	uint32_t fillColor = (filled * 100 / BAR_W > 80)
		? gp0_rgb(220, 170, 40)
		: gp0_rgb(70, 200, 110);
	if (filled > 0)
		hudRect(chain, barX, y + 1, filled, BAR_H, fillColor);

	char line[24];
	if (totalBytes < 0x100000) {
		// Sub-1MB total (SPU RAM is a fixed 512KB) - MB would round to 0,
		// so show KB instead. No fractional part needed here; a fixed
		// 512KB total and byte-granular "used" figures give a KB value
		// precise enough on its own.
		unsigned usedKB  = (unsigned) (usedBytes  / 1024);
		unsigned totalKB = (unsigned) (totalBytes / 1024);
		snprintf(line, sizeof(line), "%u/%uKB", usedKB, totalKB);
	} else {
		// Fixed-point one decimal for "used" (avoids pulling in float
		// formatting on a target with no hardware FPU); "total" is always
		// a whole MB figure (2/8/16 MB RAM tiers, 1/2 MB VRAM tiers) so it
		// doesn't need one.
		unsigned usedTenths = (unsigned) (((uint64_t) usedBytes * 10) / 0x100000);
		unsigned totalMB    = totalBytes / 0x100000;
		snprintf(line, sizeof(line), "%u.%u/%uMB",
			usedTenths / 10, usedTenths % 10, totalMB);
	}
	printString(ctx, barX + BAR_W + 3, y, 0x808080, line);
}

// Inset well clear of the screen edges on every side - CRT overscan / the
// emulator's own crop can clip a good ~8-16px border. No-op while off
// (default state - see toggleSystemHUD()).
void drawSystemHUD(RenderContext *ctx) {
	if (!hudEnabled)
		return;

	int x = ctx->screenWidth - 118;
	int y = 10;

	hudMeterRow(ctx, x, y,      "RAM ", usedRAMBytes,  detectedRAMMB * 0x100000u);
	hudMeterRow(ctx, x, y + 10, "VRAM", vramUsedBytes, vramCapacityBytes);
	hudMeterRow(ctx, x, y + 20, "SPU ", getSPURAMUsedBytes(), SPU_RAM_SIZE);
}
