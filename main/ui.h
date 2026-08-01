/*
 * ps1-ram-tester - (C) 2026 spicyjpeg
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "common/gpu.h"
#include "main/renderer.h"

typedef enum {
	// Static items
	ITEM_END       = 0,
	ITEM_SEPARATOR = 1,
	ITEM_TITLE     = 2,
	ITEM_STATIC    = 3,

	// Selectable items
	ITEM_ACTION = 4,
	ITEM_INT    = 5,
	ITEM_BINARY = 6,
	ITEM_ENUM   = 7
} MenuItemType;

struct UIState;
struct MenuItem;

typedef void (*MenuCallback)(
	RenderContext *,
	struct UIState *,
	const struct MenuItem *
);

typedef struct MenuItem {
	const char *name;
	uint8_t    type, minValue, maxValue, bitLength;

	union {
		struct {
			const char   *tag;
			MenuCallback callback;
		} action;
		struct {
			uint8_t *value;
		} int_;
		struct {
			uint8_t           *value;
			const char *const *items;
		} enum_;
	};
} MenuItem;

typedef struct UIState {
	const MenuItem *currentMenu;

	uint16_t buttons, lastButtons;
	uint16_t buttonsPressed, buttonsRepeating;
	uint8_t  repeatTimer;
	int8_t   menuCursor;
} UIState;

#ifdef __cplusplus
extern "C" {
#endif

void setupUIState(UIState *state);
void updateUIState(UIState *state, uint16_t buttons);

// Returns the index of the first selectable item (ITEM_ACTION or later
// in the type ordering) in a menu, skipping past any leading
// title/separator/static items. Use this instead of hardcoding
// menuCursor = 0, which usually points at the title item itself.
int findFirstSelectableItem(const MenuItem *menu);

void renderMenu(RenderContext *ctx, const UIState *state);
void updateMenu(RenderContext *ctx, UIState *state, uint16_t buttons);

void renderProgressScreen(
	RenderContext *ctx,
	int           progress,
	int           total,
	const char    *message
);

#ifdef __cplusplus
}
#endif
