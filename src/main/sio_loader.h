/*
 * PSX-iTests - SIO (serial) EXE loader
 *
 * Runs the hardware-proven dashboard-resident receiver: SEXE,
 * OKV2/UPV2/OKAY, header/metadata, and checksum-gated 2048-byte chunks.
 * The received payload is staged above the dashboard and only enters the
 * clean-RAM scratch handoff after the complete transfer has succeeded.
 */

#pragma once

#include "main/renderer.h"
#include "main/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

void runSIOLoader(RenderContext *ctx, UIState *state, const MenuItem *item);

#ifdef __cplusplus
}
#endif
