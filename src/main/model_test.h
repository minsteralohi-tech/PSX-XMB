/*
 * PSX-iTests - "GPU: Test PlayStation Model"
 *
 * A GTE-transformed, texture-mapped PlayStation model, ported from the
 * standalone 3D-model-test project. The camera framing and spin are
 * reproduced exactly as in that build:
 *
 *   camera distance 450, pan (0, 50)
 *   START_YAW 1824, START_PITCH 670, auto-spin +8 yaw units per frame
 *   rotation order: yaw -> pitch -> (1024, 0, 1024) to lay the model flat
 *
 * Controls: SELECT switches to manual D-pad orbit (seamlessly, from the
 * current auto-spin angle), START returns to auto-spin, any face button or
 * L/R exits back to the menu.
 */

#pragma once

#include "main/renderer.h"
#include "main/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

void runModelTest(RenderContext *ctx, UIState *state, const MenuItem *item);

#ifdef __cplusplus
}
#endif
