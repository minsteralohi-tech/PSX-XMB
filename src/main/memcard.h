/*
 * PSX-iTests - Memory Card manager
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "main/renderer.h"
#include "main/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Shared raw-sector access for small, self-contained save systems.  These
 * are the same hardware-proven SIO0 transactions used by the manager; keeping
 * the transport here avoids a second memory-card driver with subtly different
 * timing.  A sector is always exactly 128 bytes and valid ports are 0/1.
 */
bool memoryCardPresent(int port);
bool memoryCardReadSector(int port, uint16_t sector, uint8_t data[128]);
bool memoryCardWriteSector(int port, uint16_t sector, const uint8_t data[128]);

// Menu callback: BIOS-style memory card manager. Detects a card in
// either slot (Get ID, 0x53), reads all 15 block directory entries
// (Read Sector, 0x52) and shows them as a grid with a detail panel for
// the currently selected block (title, used/free/deleted status) -
// matching the actual BIOS's own layout rather than a plain list, which
// wouldn't fit this project's GPU packet budget for 15 full rows of
// text. Icon extraction is not implemented yet - the TIM/CLUT icon
// format within the save data itself needs its own separate
// verification before that's safe to build. D-PAD selects a block,
// START+SELECT together exits.
void runMemoryCardManager(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
);

#ifdef __cplusplus
}
#endif
