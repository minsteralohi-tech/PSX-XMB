/*
 * PSX-iTests - Settings -> SIO Loader (see sio_launch.c)
 *
 * Replaces the former sio_loader.h. The dashboard-resident serial receiver
 * has been removed entirely; this now hands off to the standalone SIO loader
 * PS-EXE through the two-stage launcher in app_launch.c.
 */

#pragma once

#include "main/renderer.h"
#include "main/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

void runSIOLoader(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
);

#ifdef __cplusplus
}
#endif
