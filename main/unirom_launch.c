/*
 * PSX-iTests - UniROM 8.0 launcher (see unirom_launch.h)
 */

#include <stdint.h>
#include "common/reboot.h"
#include "main/handoff.h"
#include "main/unirom_launch.h"

// The UniROM PS-EXE, embedded via addBinaryFile() in CMakeLists.txt.
extern const uint8_t uniromExe[];

__attribute__((noreturn)) void launchUniROM(void) {
	// Everything - quiescing the hardware, clearing surrounding RAM back to
	// something closer to a cold-boot state, the overlap-safe copy, the
	// cache flush and BIOS Exec - lives in handoff.c so this path and Fast
	// Boot's cannot drift apart again. handoff.h documents the hardware and
	// BIOS ownership rules that the old inline version violated.
	launchPSEXEImage(uniromExe);
}

__attribute__((noreturn)) void returnToUniROMCart(void) {
	// For consoles where a real UniROM cartridge is already installed and
	// is what booted this app in the first place: the cart intercepts the
	// BIOS ROM entry point at the hardware level, so a plain soft-reset -
	// jumping back to that entry point - lands in the CART's own resident
	// menu, not the stock Sony BIOS shell.
	//
	// launchUniROM() above is the wrong tool for this setup: it copies a
	// second, independent UniROM image into RAM and jumps into it while the
	// real cart's firmware is still resident and, on some revisions,
	// actively watching memory. That is almost certainly the source of the
	// "RAM error" - the cart's own sanity check flagging memory it expects
	// to be in a state it manages, not this app's. The correct move is to
	// not load anything at all and just hand control back the same way a
	// real reset button press would.
	quiesceForFirmwareReset();
	softReset();

	// softReset() isn't itself marked noreturn (it's a shared helper used
	// elsewhere too), so without this GCC can't prove this function - which
	// IS marked noreturn - never falls through, hence the build warning.
	// Belt and suspenders on real hardware too: if the reset somehow didn't
	// happen, sitting here is safer than falling into whatever instructions
	// follow in memory.
	for (;;) {}
}
