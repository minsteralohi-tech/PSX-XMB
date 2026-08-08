/*
 * PSX-iTests - persistent dashboard settings on a standard PS1 memory card
 */

#pragma once

#include <stdbool.h>
#include "main/renderer.h"
#include "main/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Silently loads the first valid save found (slot 1, then slot 2). */
bool loadSettingsAtBoot(void);

/* XMB Settings callback for manual save/load on either card slot. */
void runSettingsSave(
	RenderContext *ctx, UIState *state, const MenuItem *item
);

#ifdef __cplusplus
}
#endif
