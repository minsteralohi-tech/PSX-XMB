/*
 * PSX-iTests - Memory Card manager
 */

#pragma once

#include "main/renderer.h"
#include "main/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

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
