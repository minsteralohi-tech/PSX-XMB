/*
 * PSX-iTests - launch confirmation screens (see launch_ui.c)
 *
 * Replaces the former sio_loader.h and the inline confirmUniROMHandoff() that
 * lived in xmb_menu.c. Both entries now go through the same validated plan +
 * confirmation screen built on app_launch.c.
 */

#pragma once

#include "main/renderer.h"
#include "main/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Settings -> SIO Loader: the standalone serial loader. */
void runSIOLoader(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
);

/* Tools -> UniROM 8.0: the embedded real UniROM build. */
void runUniROMLauncher(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
);

/* Tools -> Sony 4.1 BIOS. */
void runSony41Bios(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
);

/* Tools -> 240p Test Suite. */
void run240pSuite(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
);

#ifdef __cplusplus
}
#endif
