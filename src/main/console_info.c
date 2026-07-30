/*
 * PSX-iTests - Console information (BIOS detection only, for now)
 *
 * Deliberately stripped down to JUST BIOS date/version/region detection.
 * The full version (BIOS + CD-ROM Mechacon + GPU + RAM size + CPU ID)
 * crashes on real hardware, even though it worked fine in DuckStation.
 * Since the very first version of this screen worked on real hardware,
 * something added afterward broke it - rather than keep guessing at
 * which piece, we're isolating them one at a time, starting with the
 * piece that's the safest by construction: BIOS detection is pure,
 * read-only memory access (KSEG1 uncached BIOS ROM space), no hardware
 * registers are written to at all, so it's the least likely piece to be
 * the actual cause - but confirming it works standalone on real hardware
 * rules it out for certain before moving on to the CD-ROM/GPU/RAM/CPU
 * checks, which all involve actual hardware register reads/writes and
 * are much more likely culprits.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "common/sio0.h"
#include "main/console_info.h"
#include "main/defs.h"
#include "main/font.h"
#include "main/mainmenu.h"
#include "main/sound.h"
#include "main/xmb_bg.h"
#include "main/ramconfig.h"
#include "ps1/registers.h"

/* ---- BIOS version/date/region ----
 *
 * CONFIRMED via psx-spx's BIOS Memory Map: BFC00100h holds a BCD-encoded
 * date (format YYYYMMDDh), and BFC00108h holds the "Kernel Maker/Version
 * Strings" (the maker name only, in practice - see getBiosVersionString()
 * below for the actual version string).
 *
 * CORRECTION: an earlier version of this code avoided the commonly-cited
 * fixed address 0xBFC7FF32 for the version string, based on The Cutting
 * Room Floor's PS1 disassembly notes claiming that offset moves between
 * BIOS revisions (SCPH-1001 at 0x42E74, SCPH-1002 at 0x423C0). That
 * turned out to be wrong: directly searching 4 real BIOS dumps (v2.2,
 * v3.0, v4.1, v4.5, spanning 1995-2000, including the exact SCPH-1001
 * BIOS TCRF was describing) found the real, actively displayed version
 * string at 0xBFC7FF32 in every single one. The "different offset" TCRF
 * found is real, but it's a generic leftover "System ROM Version 1.0"
 * template string sitting elsewhere in some BIOS builds' kernel code -
 * not the real one. Lesson: a documentation claim is still worth
 * checking directly against real files when possible, rather than taken
 * as final.
 */

static uint8_t bcdToDec(uint8_t bcd) {
	return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

static void getBiosDate(char *out, size_t outSize) {
	uint32_t value = *(const volatile uint32_t *) 0xbfc00100;

	uint8_t yearHi = (value >> 24) & 0xff;
	uint8_t yearLo = (value >> 16) & 0xff;
	uint8_t month  = (value >>  8) & 0xff;
	uint8_t day    =  value        & 0xff;

	int year = (int) bcdToDec(yearHi) * 100 + (int) bcdToDec(yearLo);

	snprintf(
		out, outSize, "%04d-%02d-%02d",
		year, bcdToDec(month), bcdToDec(day)
	);
}

static const char *getBiosMakerString(void) {
	return (const char *) 0xbfc00108;
}

// BFC00108h actually holds MULTIPLE null-terminated strings back to back
// (documented by psx-spx as "Kernel Maker/Version Strings", plural) - the
// maker name ("Sony Computer Entertainment Inc.") comes first, followed
// by the actual "System ROM Version X.X MM/DD/YY R" string. Scans forward
// through consecutive null-terminated strings looking for that prefix
// specifically, rather than assuming the first string is the right one.
static bool stringStartsWith(const char *str, const char *prefix) {
	while (*prefix) {
		if (*str != *prefix)
			return false;
		str++;
		prefix++;
	}
	return true;
}

static const char *getBiosVersionString(void) {
	// CONFIRMED directly against 4 real BIOS dumps (v2.2, v3.0, v4.1,
	// v4.5, spanning 1995-2000): the active, displayed version string is
	// reliably at this exact address in every one of them. An earlier
	// round of research suggested this address wasn't reliable and moved
	// between revisions - that turned out to be based on a false
	// positive (a generic leftover "System ROM Version 1.0" template
	// string that exists elsewhere in some BIOS builds' kernel code, not
	// the real, displayed one). This was verified by directly searching
	// real BIOS files rather than assumed from documentation.
	const char *direct = (const char *) 0xbfc7ff32;
	if (stringStartsWith(direct, "System ROM Version"))
		return direct;

	// Fallback: scan the whole BIOS ROM region for the string, in case
	// some exotic/unusual BIOS revision genuinely differs from every
	// version actually tested. 512KB scanned a byte at a time is still
	// well under a second even on the original CPU, so this is safe to
	// do live if it's ever actually needed - which, per the above, it
	// shouldn't be for any real Sony BIOS.
	const char *base = (const char *) 0xbfc00000;
	for (uint32_t offset = 0; offset < (0x80000 - 19); offset++) {
		if (stringStartsWith(base + offset, "System ROM Version"))
			return base + offset;
	}

	// Genuinely not found anywhere - fall back to the maker string so
	// there's still something reasonable to display.
	return getBiosMakerString();
}

// The region letter is the last non-whitespace character of the version
// string (e.g. "...12/04/95 A" -> 'A').
static char getBiosRegionLetter(void) {
	const char *str = getBiosVersionString();

	size_t len = 0;
	while (str[len])
		len++;

	while (len > 0) {
		char c = str[len - 1];
		if ((c != ' ') && (c != '\r') && (c != '\n'))
			break;
		len--;
	}

	return (len > 0) ? str[len - 1] : '?';
}

static const char *regionName(char letter) {
	switch (letter) {
		case 'A': return "Americas (NTSC)";
		case 'E': return "Europe (PAL)";
		case 'I': return "Japan (early NTSC-J)";
		case 'J': return "Japan (NTSC-J)";
		default:  return "Unknown/non-standard";
	}
}

/* ---- CD-ROM controller (Mechacon) version -> motherboard revision ----
 *
 * REBUILT against real, production reference code: tonyhax
 * (github.com/socram8888/tonyhax), a save-exploit loader that has to work
 * reliably across essentially every real PS1 in the wild - about as
 * battle-tested as PS1 bare-metal code gets. Comparing our earlier
 * implementation against their cd_command()/cd_wait_int() revealed a real,
 * likely-causal bug: we never waited for BUSYSTS (bit 7 of the status
 * register) to clear before sending a new command or before polling for
 * the response interrupt. tonyhax does this as the very first step in
 * both functions. We also only cleared the parameter FIFO (0x40) where
 * tonyhax clears both the parameter FIFO AND acknowledges BUSYSTS
 * together (0xC0). Missing synchronization like this is exactly the kind
 * of thing that a lenient emulator can tolerate while real hardware
 * can't - a very plausible explanation for instability that only showed
 * up on real consoles.
 *
 * Also confirmed independently correct by this reference: reading the
 * response FIFO while bit 5 (RSLRRDY) is set, which is what our own
 * fix already landed on.
 */

#define CDROM_REG0 (*(volatile uint8_t *) 0x1f801800) // status/index
#define CDROM_REG1 (*(volatile uint8_t *) 0x1f801801) // command / response fifo
#define CDROM_REG2 (*(volatile uint8_t *) 0x1f801802) // parameter fifo / data fifo
#define CDROM_REG3 (*(volatile uint8_t *) 0x1f801803) // request / interrupt enable/flag

#define CDROM_STAT_RSLRRDY (1 << 5)
#define CDROM_STAT_BUSYSTS (1 << 7)

static void cdromSendCommand(uint8_t cmd, const uint8_t *params, int numParams) {
	// Wait for any previous command to finish - this was the missing step.
	while (CDROM_REG0 & CDROM_STAT_BUSYSTS)
		;

	CDROM_REG0 = 0; // page 0

	// Clear the parameter FIFO AND acknowledge BUSYSTS together (0xC0),
	// not just the parameter FIFO alone (0x40, what we had before).
	CDROM_REG3 = 0xc0;

	for (int i = 0; i < numParams; i++)
		CDROM_REG2 = params[i];

	CDROM_REG0 = 1;    // page 1
	CDROM_REG2 = 0x00; // disable IRQ generation - we're polling manually
	CDROM_REG3 = 0x07; // acknowledge any pending interrupt flags
	CDROM_REG0 = 0;    // back to page 0

	CDROM_REG1 = cmd; // trigger the command
}

// Returns the interrupt code (1-7), or -1 on timeout.
static int cdromWaitForInterrupt(void) {
	while (CDROM_REG0 & CDROM_STAT_BUSYSTS)
		;

	CDROM_REG0 = 1;

	uint32_t timeout = 2000000;
	uint8_t  flags;
	do {
		flags = CDROM_REG3 & 0x07;
		if (--timeout == 0)
			return -1;
	} while (flags == 0);

	CDROM_REG3 = 0x07; // acknowledge
	return flags;
}

static int cdromReadResponse(uint8_t *out, int maxBytes) {
	CDROM_REG0 = 1;

	int count = 0;
	while ((count < maxBytes) && (CDROM_REG0 & CDROM_STAT_RSLRRDY))
		out[count++] = CDROM_REG1;

	return count;
}

typedef struct {
	uint8_t     bytes[4];
	const char  *motherboard;
	const char  *date;
} MechaconEntry;

// Taken directly from psx-spx's own published Mechacon version table, plus
// one entry confirmed against real hardware (95 09 12 C2).
static const MechaconEntry MECHACON_TABLE[] = {
	{ { 0x94, 0x09, 0x19, 0xc0 }, "PU-7",          "1994-09-19" },
	{ { 0x94, 0x11, 0x18, 0xc0 }, "PU-7",          "1994-11-18" },
	{ { 0x95, 0x05, 0x16, 0xc1 }, "LATE-PU-8",     "1995-05-16" },
	{ { 0x95, 0x07, 0x24, 0xc1 }, "LATE-PU-8",     "1995-07-24" },
	{ { 0x95, 0x09, 0x12, 0xc2 }, "LATE-PU-8",     "1995-09-12" },
	{ { 0x96, 0x08, 0x15, 0xc2 }, "PU-16 (VCD)",   "1996-08-15" },
	{ { 0x96, 0x08, 0x18, 0xc1 }, "LATE-PU-8",     "1996-08-18" },
	{ { 0x96, 0x09, 0x12, 0xc2 }, "PU-18 (JP)",    "1996-09-12" },
	{ { 0x97, 0x01, 0x10, 0xc2 }, "PU-18",         "1997-01-10" },
	{ { 0x97, 0x08, 0x14, 0xc2 }, "PU-20",         "1997-08-14" },
	{ { 0x98, 0x06, 0x10, 0xc3 }, "PU-22",         "1998-06-10" },
	{ { 0x99, 0x02, 0x01, 0xc3 }, "PU-23 / PM-41", "1999-02-01" },
	{ { 0xa1, 0x03, 0x06, 0xc3 }, "PM-41 (later)", "2001-06-06" },
};

#define NUM_MECHACON_ENTRIES (sizeof(MECHACON_TABLE) / sizeof(MECHACON_TABLE[0]))

// Returns true on success. intCode reports what actually happened (3 =
// success, other INT codes or -1 for timeout = something went wrong) so
// the display can show it even on failure, for diagnostic purposes.
static bool getCDROMVersion(uint8_t out[4], int *intCode) {
	uint8_t param = 0x20; // GetROM sub-function

	cdromSendCommand(0x19, &param, 1);
	int interrupt = cdromWaitForInterrupt();
	*intCode = interrupt;

	if (interrupt < 0) {
		out[0] = out[1] = out[2] = out[3] = 0;
		return false;
	}

	uint8_t raw[16];
	int     count = cdromReadResponse(raw, sizeof(raw));

	if ((interrupt != 3) || (count != 4)) {
		out[0] = out[1] = out[2] = out[3] = 0;
		return false;
	}

	out[0] = raw[0];
	out[1] = raw[1];
	out[2] = raw[2];
	out[3] = raw[3];
	return true;
}

static const MechaconEntry *lookupMechacon(const uint8_t bytes[4]) {
	for (size_t i = 0; i < NUM_MECHACON_ENTRIES; i++) {
		const uint8_t *entry = MECHACON_TABLE[i].bytes;

		if (
			(entry[0] == bytes[0]) && (entry[1] == bytes[1]) &&
			(entry[2] == bytes[2]) && (entry[3] == bytes[3])
		)
			return &MECHACON_TABLE[i];
	}

	return NULL;
}

/* ---- Main RAM size (fold test) ----
 *
 * Standard "memory fold" test, extended to check multiple boundaries
 * instead of just the usual 2MB-vs-8MB check: write a marker to the very
 * start of RAM, then a different marker at each candidate size boundary
 * in increasing order. If a given boundary is beyond the console's real,
 * physically wired memory, that write "folds" back and overwrites the
 * start of RAM too (the address bus doesn't have enough lines to
 * distinguish it), which we detect by reading the start value back. The
 * first boundary where a fold shows up is the real RAM size - larger
 * boundaries aren't tested once that's found, since they'd fold for the
 * same reason.
 *
 * This is NOT the same kind of test as ps1-ram-tester's own RAM test -
 * that one is designed to find bad/failing memory cells and needs many
 * passes with different bit patterns to catch intermittent faults. This
 * is answering a completely different question ("how much RAM is
 * physically here"), which only takes a handful of writes/reads, no
 * passes needed at all.
 */

/* ---- Main RAM size ----
 *
 * REPLACED the earlier memory-fold approach entirely - it was based on a
 * wrong assumption. The PS1 memory controller's address folding isn't
 * purely a function of which RAM chips are physically installed; it's
 * governed by the DRAM_CTRL configuration register, which the BIOS sets
 * up at boot. That register can be WRONG relative to what's physically
 * there - ramconfig.c's own fixRetailRAMConfig() comment says exactly
 * this: "The retail BIOS accidentally configures main RAM as 8 MB." That
 * mismatch is what caused both symptoms: DuckStation showing 16MB (no
 * real fold ever occurred, since the controller was configured for more
 * space than physically exists) and real hardware crashing (writing into
 * that "phantom" region isn't a safe fold, it's addresses with no real
 * memory chip behind them).
 *
 * The actual correct approach, already proven on real hardware: read
 * DRAM_CTRL directly, exactly what ps1-ram-tester's own getMainRAMSize()
 * already does (see ramconfig.c) - a plain status register read, no
 * memory writes at all, so there's nothing left to corrupt.
 */

/* ---- GPU version (needed to pick the correct 16MB-unlock value) ---- */

static bool isNewGPU(void) {
	GPU_GP1 = 0x10000007; // GP1(10h), index 7

	return GPU_GP0 == 2; // GPUREAD, same physical register as GP0
}

/* ---- 16MB RAM unlock ----
 *
 * Community-sourced technique from a real PS1 RAM-modding hobbyist,
 * independently cross-checked against our own pre-existing DRAM_CTRL
 * decode formula (getMainRAMSize() above) before trusting it: writing
 * 0x0FAC (later-GPU boards) or 0x0FA4 (early-GPU boards) to DRAM_CTRL
 * both decode to exactly 16MB through that same formula - real,
 * independent confirmation this is accurate, not taken on faith. Also
 * confirmed directly: a real 16MB-modded console reported
 * DRAM_CTRL=0x8C430988 (decodes to 2MB) before this write - the stock
 * kernel never configures DRAM_CTRL for 16MB on its own, it has to be
 * written explicitly.
 *
 * This writes to a configuration register - the same one
 * fixRetailRAMConfig() already safely writes to, just a different value
 * - not raw memory, so it's inherently safer than the earlier raw fold
 * test that crashed by touching kernel-reserved memory. Still: telling
 * the memory controller there's more RAM than a normal console has is
 * not risk-free if that RAM genuinely isn't there, so this verifies with
 * a proper fold-detection check (not just a write/readback, which could
 * give a false positive if the write happens to fold onto an
 * undisturbed address) before trusting the change, and always restores
 * the original value if verification fails.
 */

// Writes 6 distinct values across 6 addresses spread through the given
// region, then checks all of them independently retained their own
// distinct value - if they're secretly all aliasing to the same
// physical location, only the last value written would survive
// everywhere, so six different addresses can't coincidentally all pass
// unless genuinely backed by independent memory.
static bool verifyDistinctRegion(const uint32_t offsets[6]) {
	static const uint32_t patterns[6] = {
		0x11111111, 0x22222222, 0x33333333,
		0x44444444, 0x55555555, 0x66666666
	};

	volatile uint32_t *ptrs[6];
	uint32_t           saved[6];

	for (int i = 0; i < 6; i++) {
		ptrs[i]  = (volatile uint32_t *) (0x80000000 + offsets[i]);
		saved[i] = *ptrs[i];
	}

	for (int i = 0; i < 6; i++)
		*ptrs[i] = patterns[i];

	bool allDistinct = true;
	for (int i = 0; i < 6; i++) {
		if (*ptrs[i] != patterns[i]) {
			allDistinct = false;
			break;
		}
	}

	for (int i = 0; i < 6; i++)
		*ptrs[i] = saved[i];

	return allDistinct;
}

// Addresses spread through the claimed 8-16MB extension (for testing the
// 16MB unlock) and through the claimed 2-8MB extension (for testing
// plain 8MB), each safely clear of the boundary edges on either side.
static const uint32_t REGION_8_TO_16MB[6] = {
	0x00900000, 0x00a00000, 0x00b00000,
	0x00d00000, 0x00e00000, 0x00e80000
};
static const uint32_t REGION_2_TO_8MB[6] = {
	0x00300000, 0x00380000, 0x00400000,
	0x00500000, 0x00600000, 0x00700000
};

// Sequential test: try claiming 16MB first, then 8MB, falling back to
// the always-valid 2MB baseline - each tier verified independently
// before being trusted.
//
// FIXED a real bug here: the first version wrote hardcoded, size-only
// values for the 8MB and 2MB-fallback steps (DRAM_CTRL_SIZE_8MB |
// DRAM_CTRL_BANKS_1, etc.), which strips out the OTHER bits in the
// register - and DRAM_CTRL doesn't just encode size, it also encodes
// memory timing parameters specific to the actual RAM chips on that
// board. Overwriting those with a size-only reconstruction destroys the
// real timing configuration the BIOS originally set up. An emulator
// doesn't model those timing bits at all, so this passed fine on
// DuckStation - but real hardware genuinely needs them, which is why it
// crashed there. Same principle fixRetailRAMConfig() already follows:
// read-modify-write, only touching the size/bank bits, preserving
// everything else. The 16MB magic values are exempt from this - your
// friend's values already include the correct timing bits for that
// specific configuration, confirmed earlier by cross-checking their
// size-decode against our own formula.
static uint32_t tryProgressiveRAMDetect(void) {
	uint32_t original = DRAM_CTRL;
	uint32_t detected = 2;

	DRAM_CTRL = isNewGPU() ? 0x0fac : 0x0fa4;
	if (verifyDistinctRegion(REGION_8_TO_16MB)) {
		detected = 16;
	} else {
		DRAM_CTRL = (original & ~(DRAM_CTRL_SIZE_BITMASK | DRAM_CTRL_BANKS_BITMASK))
		          | DRAM_CTRL_SIZE_8MB | DRAM_CTRL_BANKS_1;
		if (verifyDistinctRegion(REGION_2_TO_8MB))
			detected = 8;
	}

	/*
	 * ALWAYS restore the exact original value, even when extra RAM was
	 * found. This page only needs to *report* the installed size - it must
	 * not leave the memory controller reconfigured behind the user's back.
	 *
	 * Leaving DRAM_CTRL modified here corrupted everything downstream that
	 * hands control back to the BIOS: doFastReboot() passes the live
	 * DRAM_CTRL value to softFastRebootWithConfig(), so a probe-modified
	 * register made "Normal Boot" hang, and softReset() likewise handed the
	 * BIOS a memory map it never configured, producing the garbled Sony
	 * logo on full reboot. Restoring here fixes both.
	 *
	 * Deliberately configuring the RAM controller is still available, and
	 * still explicit, via the RAM Tester's "Configure Main RAM" submenu.
	 */
	DRAM_CTRL = original;
	return detected;
}

// Public wrapper: same probe, used by the boot-time HUD/badge detection in
// hud.c so it doesn't need its own copy of the progressive-tier logic.
uint32_t detectPhysicalRAMSizeMB(void) {
	return tryProgressiveRAMDetect();
}


/* ---- Physical RAM size probe for the RAM tester ----
 *
 * Answers "how much of the CURRENTLY-configured main RAM is actually backed
 * by real, distinct memory chips" WITHOUT modifying DRAM_CTRL and without the
 * multi-megabyte destructive pattern loop that ps1-ram-tester's testMainRAM()
 * runs.
 *
 * Why this exists: the RAM tester derives its test range purely from
 * getMainRAMSize() (a DRAM_CTRL read). If the user opens "Configure main
 * RAM", picks 4 MB or 8 MB and applies it on a console that only physically
 * has 2 MB, DRAM_CTRL now decodes that larger space - but there are no chips
 * behind the upper addresses. testMainRAM() then hammers that "phantom"
 * region: on real hardware, driving the memory controller for RAM that isn't
 * there doesn't cleanly fold, it stalls the bus / corrupts the low RAM the
 * running program itself lives in, which is the "stuck in testing with no
 * error" hang. (In emulators it folds and used to surface as a mismatch.)
 * Testing memory that isn't physically present is never valid, so we clamp
 * the destructive test to what's really there.
 *
 * The probe itself is the exact six-point, save-and-restore verifyDistinctRegion()
 * check the Console Information page already uses and that is confirmed safe on
 * real 2 MB hardware (it correctly reports 2 MB there without hanging). It only
 * touches a handful of addresses, never DRAM_CTRL, so the user's applied
 * configuration is left exactly as they set it.
 *
 * Note on 4 MB: 4 MB main-RAM configurations are non-standard and physically-4MB
 * PS1s essentially don't exist, and the region tables above are cut for the real
 * 2/8/16 MB tiers, so a 4 MB configuration that isn't backed by >=8 MB simply
 * clamps to the always-present 2 MB baseline. That is exactly the safe outcome
 * the user wants for "2 MB console configured as 4 MB" - it tests the real 2 MB
 * instead of hanging.
 */
size_t probePhysicalMainRAMSize(size_t configuredBytes) {
	// 2 MB (or less) is genuinely present on every PS1 ever made and is where
	// the running program lives - always safe to test, never probe below it.
	if (configuredBytes <= 0x200000)
		return configuredBytes;

	// Probe only tiers that (a) don't exceed what the controller is currently
	// decoding and (b) verify as real distinct memory. verifyDistinctRegion()
	// writes six spread-out markers and confirms each kept its own value; a
	// phantom region folds back onto low RAM so at least two markers collide
	// and it returns false.
	if ((configuredBytes >= 0x1000000) && verifyDistinctRegion(REGION_8_TO_16MB))
		return 0x1000000;                  // genuine 16 MB present
	if ((configuredBytes >= 0x800000)  && verifyDistinctRegion(REGION_2_TO_8MB))
		return 0x800000;                   // genuine 8 MB present

	// Anything the controller claims beyond 2 MB that didn't verify is phantom.
	return 0x200000;
}


/* ---- Combined console information page ----
 *
 * Everything the old separate BIOS / CD-ROM / RAM-size / GPU-version screens
 * reported, gathered once when the page opens and shown on a single screen.
 * The RAM size is probed automatically here (no key press, no DRAM_CTRL
 * internals on screen) - tryProgressiveRAMDetect() verifies each tier before
 * trusting it and restores the original controller value if nothing beyond
 * the 2MB baseline checks out.
 */

// Human-readable meaning of a detected main-RAM size.
static const char *ramSizeDescription(uint32_t sizeMB) {
	switch (sizeMB) {
		case 16: return "Development / Modified Console";
		case  8: return "Development Kit / Net Yaroze";
		default: return "Standard Retail Console";
	}
}

void runConsoleInfo(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
) {
	(void) state;
	(void) item;

	// ---- gather everything once, up front ----
	char biosDate[16];
	getBiosDate(biosDate, sizeof(biosDate));
	const char *biosVersion      = getBiosVersionString();
	char        biosRegionLetter = getBiosRegionLetter();

	uint8_t mechacon[4];
	int     cdIntCode;
	bool    cdOK = getCDROMVersion(mechacon, &cdIntCode);
	const MechaconEntry *mechaconInfo = cdOK ? lookupMechacon(mechacon) : NULL;

	// Probed automatically on entry, exactly like the other readings.
	uint32_t ramMB = tryProgressiveRAMDetect();

	bool gpuIsNew = isNewGPU();

	char line[64];

	// Debounce: wait for the button that opened this screen to be released.
	while (pollController(0) | pollController(1))
		;

	for (;;) {
		uint16_t pad = pollController(0) | pollController(1);
		if (pad & PAD_BTN_CIRCLE) {
			playCancelSound();
			break;
		}

		beginFrame(ctx);
		drawXMBBackground(ctx);

		printString(ctx, 16, 14, 0x808080, "CONSOLE INFORMATION");

		// ---- BIOS ----
		printString(ctx, 16, 32, 0x505050, "BIOS");
		snprintf(
			line, sizeof(line), "Date: %s   Region: %c (%s)",
			biosDate, biosRegionLetter, regionName(biosRegionLetter)
		);
		printString(ctx, 24, 42, 0xffffff, line);
		printString(ctx, 24, 52, 0xffffff, biosVersion);

		// ---- CD-ROM controller ----
		printString(ctx, 16, 68, 0x505050, "CD-ROM Controller");
		if (cdOK) {
			snprintf(
				line, sizeof(line), "Mechacon: %02X %02X %02X %02X",
				mechacon[0], mechacon[1], mechacon[2], mechacon[3]
			);
			printString(ctx, 24, 78, 0xffffff, line);

			if (mechaconInfo) {
				snprintf(
					line, sizeof(line), "%s  (%s)",
					mechaconInfo->motherboard, mechaconInfo->date
				);
				printString(ctx, 24, 88, 0xffffff, line);
			} else {
				printString(ctx, 24, 88, 0x808080, "Unrecognized controller version");
			}
		} else {
			printString(
				ctx, 24, 78, 0x808080,
				(cdIntCode < 0) ? "No response (timed out)" : "Command did not succeed"
			);
		}

		// ---- RAM ----
		printString(ctx, 16, 104, 0x505050, "RAM");
		snprintf(line, sizeof(line), "Detected: %u MB", (unsigned int) ramMB);
		printString(ctx, 24, 114, 0xffffff, line);
		printString(ctx, 24, 124, 0xffffff, ramSizeDescription(ramMB));

		// ---- GPU ----
		printString(ctx, 16, 140, 0x505050, "GPU Version");
		printString(
			ctx, 24, 150, 0xffffff,
			gpuIsNew
				? "208-pin GPU (LATE-PU-8 and up)"
				: "160-pin GPU (EARLY-PU-8 and below)"
		);
		if (mechaconInfo) {
			snprintf(line, sizeof(line), "Motherboard: %s", mechaconInfo->motherboard);
			printString(ctx, 24, 160, 0xffffff, line);
		}

		printString(ctx, 16, 218, 0x606060, CH_PS1_CIRCLE_BUTTON " Back");

		endFrame(ctx);
	}

	// Flush button state before handing control back to the outer menu
	// system - see the matching comment in memcard.c's
	// runMemoryCardManager() for the full explanation.
	while (pollController(0) | pollController(1))
		;
}
