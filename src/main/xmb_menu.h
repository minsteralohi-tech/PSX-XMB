/*
 * PSX-iTests - PSP/PS3-style XMB cross menu
 *
 * Replaces the top-level vertical list with a horizontal category ribbon and a
 * vertical item list, with icons, drop shadows and smooth gliding. Selecting an
 * item dispatches to the existing screen/test callbacks; submenus and tests
 * still use the classic list UI (handled by the normal renderMenu/updateMenu
 * path). enterMainMenu() re-activates the XMB, so any "back to main menu"
 * returns here.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "main/renderer.h"
#include "main/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

void initXMB(void);
bool isXMBActive(void);
void setXMBActive(bool active);

void renderXMB(RenderContext *ctx, UIState *state);
void updateXMB(RenderContext *ctx, UIState *state, uint16_t buttons);

#ifdef __cplusplus
}
#endif
