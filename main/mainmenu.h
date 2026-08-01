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

#include <stdbool.h>
#include <stdint.h>
#include "main/renderer.h"
#include "main/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

void enterMainMenu(RenderContext *ctx, UIState *state, const MenuItem *item);

// Tools submenu - houses the debug/diagnostic screens and CD player,
// kept separate from the main menu so the top-level screen doesn't get
// cluttered as more of these get added.
void enterToolsMenu(RenderContext *ctx, UIState *state, const MenuItem *item);

// True if the top-level main menu is the currently active screen (as
// opposed to a submenu like the RAM tester, or any test screen). Used to
// gate the BGM/SFX toggle shortcuts (L2/R2) to only work there, so they
// don't interfere with other screens that use L2/R2 for their own
// purposes (e.g. the pad tester's rumble test).
bool isMainMenuActive(const UIState *state);

#ifdef __cplusplus
}
#endif
