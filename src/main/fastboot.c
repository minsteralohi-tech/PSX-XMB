/*
 * PSX-iTests - Fast Boot loader launcher (see fastboot.h)
 */

#include <stdint.h>
#include "main/fastboot.h"
#include "main/handoff.h"

// The loader PS-EXE, embedded via addBinaryFile() in CMakeLists.txt.
extern const uint8_t cdloaderExe[];

__attribute__((noreturn)) void launchLoader(void) {
	// Shared with Tools -> UniROM 8.0; see handoff.h. cdloader.exe happened
	// to survive the old hand-off because its own setupGPU() clears the
	// stuck GPU DMA channel on start-up, but it should never have been
	// relying on that.
	launchPSEXEImage(cdloaderExe);
}
