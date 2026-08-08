/*
 * PSX-iTests - Controller (pad) tester
 *
 * Built entirely on the already-proven common/sio0.c primitives
 * (exchangeSIO0Packet, selectControllerPort) - no changes to that shared
 * file, same "self-contained" approach used for the GPU cube test.
 *
 * The digital button decoding matches this project's own already-working
 * pollController() exactly (response[2] = low byte, response[3] = high
 * byte, XORed since the pad reports buttons active-low). The analog stick
 * byte positions (response[4..7] = Right X, Right Y, Left X, Left Y) and
 * the exact command byte sequences for entering config mode, forcing
 * analog mode, and mapping the rumble motors are taken from a reference
 * pad tester application (Shendo/ggrtk's PadTest, PSXSDK-based) the user
 * provided - that project's rumble/config code was never actually
 * triggered by anything in its own main loop, so only the byte sequences
 * are reused here; the logic for when to run them and how the player
 * controls the motors is new.
 *
 * IMPORTANT: the config-mode handshake responses (steps 1-4 below) do NOT
 * follow the same "byte 0 = pad type nibble" format a normal poll response
 * does. Treating them as if they did (checking for "disconnection" on
 * every response, regardless of what command was sent) was misreading
 * config-mode responses as a disconnected pad, resetting the handshake
 * and restarting it from scratch forever - it never once reached the code
 * that reads real button data. The fix: only interpret/validate the
 * response as a type+button poll when we actually sent a normal poll
 * command. Config-mode steps just blindly advance, matching how the
 * reference source's own ReadPad() only checks for disconnection in its
 * default (normal poll) case, never in the config-mode cases.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "common/sio0.h"
#include "main/defs.h"
#include "main/font.h"
#include "main/icon.h"
#include "main/mainmenu.h"
#include "main/xmb_bg.h"
#include "main/pad_test.h"
#include "main/sound.h"
#include "ps1/gpucmd.h"

#define HIGHLIGHT_COLOR 0xe04040 // red, matching the Circle button's color

typedef struct {
	bool     connected;
	uint8_t  type;
	uint16_t buttons;
	uint8_t  rightX, rightY, leftX, leftY;
	int      configStep; // 0 = normal polling, 1-4 = one-time config handshake
} PadState;

// Config handshake, byte sequences taken from the reference PadTest
// application (controllers.c). Each step is one full SIO0 exchange; the
// handshake advances one step per pollPad() call rather than blocking, so
// it takes 4 frames to complete. Safe to run against pads that don't
// support it (plain digital pads just don't respond meaningfully and stay
// digital).
static size_t buildConfigRequest(int step, uint8_t *request) {
	switch (step) {
		case 1: // Enter config mode
			request[0] = SIO0_PAD_CONFIG_MODE;
			request[1] = 0x00;
			request[2] = 0x01;
			request[3] = 0x00;
			return 4;

		case 2: // Force analog mode, locked (won't toggle back via ANALOG button)
			request[0] = SIO0_CFG_SET_ANALOG;
			request[1] = 0x00;
			request[2] = 0x01;
			request[3] = 0x03;
			request[4] = 0x00;
			request[5] = 0x00;
			request[6] = 0x00;
			request[7] = 0x00;
			return 8;

		case 3: // Map rumble motors: byte 2 of poll requests -> motor 0 (small)
			request[0] = SIO0_CFG_REQUEST_SETUP;
			request[1] = 0x00;
			request[2] = 0x00;
			request[3] = 0x01;
			request[4] = 0xff;
			request[5] = 0xff;
			request[6] = 0xff;
			request[7] = 0xff;
			return 8;

		default: // Exit config mode
			request[0] = SIO0_PAD_CONFIG_MODE;
			request[1] = 0x00;
			request[2] = 0x00;
			request[3] = 0x00;
			request[4] = 0x00;
			request[5] = 0x00;
			request[6] = 0x00;
			request[7] = 0x00;
			return 8;
	}
}

static void pollPad(
	int       port,
	PadState  *pad,
	uint8_t   smallMotor,
	uint8_t   bigMotor
) {
	uint8_t request[8], response[8];
	size_t  reqLength;

	bool isConfigStep = (pad->configStep >= 1) && (pad->configStep <= 4);

	if (isConfigStep) {
		reqLength = buildConfigRequest(pad->configStep, request);
	} else {
		request[0] = SIO0_PAD_POLL;
		request[1] = 0x00;
		request[2] = smallMotor; // Actuator control 1
		request[3] = bigMotor;   // Actuator control 2
		reqLength  = 4;
	}

	selectControllerPort(port);
	size_t respLength = exchangeSIO0Packet(
		SIO0_ADDR_CONTROLLER, request, response, reqLength, sizeof(response)
	);

	if (isConfigStep) {
		// Don't try to interpret this response at all - just advance. We
		// already know a pad is connected (that's the only reason we
		// started the handshake), and this response doesn't necessarily
		// use the same format a normal poll does.
		pad->configStep++;
		if (pad->configStep > 4)
			pad->configStep = 0;

		return;
	}

	// Normal poll response: this DOES use the byte0=type-nibble format.
	if ((respLength < 4) || ((response[0] >> 4) == PAD_TYPE_NONE)) {
		pad->connected  = false;
		pad->type       = PAD_TYPE_NONE;
		pad->buttons    = 0;
		pad->configStep = 0;
		return;
	}

	uint8_t detectedType = response[0] >> 4;
	bool    typeChanged  = !pad->connected || (pad->type != detectedType);

	pad->connected = true;
	// Record the type immediately, even when this triggers a new handshake
	// below - otherwise the next poll would still see a "mismatch" against
	// the stale old value and kick off the handshake all over again,
	// forever, exactly as just happened.
	pad->type = detectedType;

	if (typeChanged) {
		// Newly connected, or type changed (e.g. player pressed the ANALOG
		// button) - kick off the config handshake. Buttons will start
		// updating again on the next normal poll after it completes.
		pad->configStep = 1;
	} else {
		pad->buttons = (response[2] | (response[3] << 8)) ^ 0xffff;

		if (
			(respLength >= 8) &&
			((detectedType == PAD_TYPE_ANALOG_STICK) || (detectedType == PAD_TYPE_ANALOG))
		) {
			pad->rightX = response[4];
			pad->rightY = response[5];
			pad->leftX  = response[6];
			pad->leftY  = response[7];
		}
	}
}

/* Button combo readout, e.g. "X+O+R1" */

static const struct { uint16_t mask; const char *name; } BUTTON_NAMES[] = {
	{ PAD_BTN_UP,       CH_PS1_DPAD_UP            },
	{ PAD_BTN_DOWN,     CH_PS1_DPAD_DOWN          },
	{ PAD_BTN_LEFT,     CH_PS1_DPAD_LEFT          },
	{ PAD_BTN_RIGHT,    CH_PS1_DPAD_RIGHT         },
	{ PAD_BTN_L1,       CH_PS1_L1_BUTTON         },
	{ PAD_BTN_L2,       CH_PS1_L2_BUTTON         },
	{ PAD_BTN_L3,       CH_PS1_ANALOG_STICK      },
	{ PAD_BTN_R1,       CH_PS1_R1_BUTTON         },
	{ PAD_BTN_R2,       CH_PS1_R2_BUTTON         },
	{ PAD_BTN_R3,       CH_PS1_ANALOG_STICK      },
	{ PAD_BTN_SELECT,   CH_PS1_SELECT_BUTTON     },
	{ PAD_BTN_START,    CH_PS1_START_BUTTON      },
	{ PAD_BTN_TRIANGLE, CH_PS1_TRIANGLE_BUTTON   },
	{ PAD_BTN_CIRCLE,   CH_PS1_CIRCLE_BUTTON     },
	{ PAD_BTN_CROSS,    CH_PS1_CROSS_BUTTON      },
	{ PAD_BTN_SQUARE,   CH_PS1_SQUARE_BUTTON     }
};

#define NUM_BUTTON_NAMES (sizeof(BUTTON_NAMES) / sizeof(BUTTON_NAMES[0]))

static void buildComboString(uint16_t buttons, char *out, size_t outSize) {
	size_t pos   = 0;
	bool   first = true;

	for (size_t i = 0; i < NUM_BUTTON_NAMES; i++) {
		if (!(buttons & BUTTON_NAMES[i].mask))
			continue;

		int n = snprintf(
			out + pos, outSize - pos, "%s%s",
			first ? "" : "+", BUTTON_NAMES[i].name
		);
		if (n > 0)
			pos += (size_t) n;

		first = false;
	}

	if (first && outSize)
		out[0] = '\0';
}

/* Drawing */

#define HALF_WIDTH 160

/*
 * Every button on this screen is now drawn as a pad glyph with its own
 * pressed artwork (drawPadButton below). The previous helpers here -
 * drawButtonGlyph, which put a HIGHLIGHT_COLOR box behind a font glyph, and
 * drawLabeledButton, which drew a box with a text label - are gone along
 * with the boxes themselves.
 */

/*
 * A pad glyph that swaps artwork when pressed.
 *
 * There is no highlight box behind it: the pressed state IS a different
 * piece of art (the lit version of the same button), so a coloured backdrop
 * would only fight with it. That also means these glyphs read correctly at
 * a glance without needing a text label underneath.
 */
static void drawPadButton(
	RenderContext *ctx, int x, int y, PadGlyph glyph, bool pressed
) {
	drawPadGlyph(ctx, glyph + (pressed ? PAD_GLYPH_PRESSED : 0), x, y);
}

/*
 * D-pad cross, built from the four direction glyphs.
 *
 * These are four separate pieces of artwork rather than one rotated glyph.
 * font.c still has printDpadDirection() for the rotated single-glyph version,
 * which is what inline text like the XMB footer hint uses - but here each
 * direction also needs a distinct pressed variant, so drawing them as four
 * cells is both simpler and what the artwork provides.
 *
 * Spacing is 16px from centre: the glyph cells are 18x16, so this keeps the
 * arms visually touching at the centre the way a real D-pad does.
 */
static void drawDpadIcon(
	RenderContext *ctx,
	int           centerX,
	int           centerY,
	uint16_t      buttons
) {
	int hw = PAD_GLYPH_W / 2, hh = PAD_GLYPH_H / 2;

	drawPadButton(ctx, centerX - hw, centerY - hh - 14,
		PAD_GLYPH_DPAD_UP,    buttons & PAD_BTN_UP);
	drawPadButton(ctx, centerX - hw, centerY - hh + 14,
		PAD_GLYPH_DPAD_DOWN,  buttons & PAD_BTN_DOWN);
	drawPadButton(ctx, centerX - hw - 14, centerY - hh,
		PAD_GLYPH_DPAD_LEFT,  buttons & PAD_BTN_LEFT);
	drawPadButton(ctx, centerX - hw + 14, centerY - hh,
		PAD_GLYPH_DPAD_RIGHT, buttons & PAD_BTN_RIGHT);
}

static void drawAnalogStick(
	RenderContext *ctx,
	int           centerX,
	int           centerY,
	uint8_t       rawX,
	uint8_t       rawY,
	bool          active,
	bool          clicked
) {
	// 14, not 16: the indicator is now an 18x16 glyph rather than a 6x6
	// dot, so at full deflection it extends 8px further than the cross arm.
	// This keeps its lowest edge clear of the live pressed-button row.
	const int radius = 14;

	drawRect(ctx, centerX - radius, centerY - 1, radius * 2, 2, 0x383838, false);
	drawRect(ctx, centerX - 1, centerY - radius, 2, radius * 2, 0x383838, false);

	int dotX = centerX;
	int dotY = centerY;

	if (active) {
		dotX += ((int) rawX - 128) * radius / 128;
		dotY += ((int) rawY - 128) * radius / 128;
	}

	// The stick glyph IS the indicator: it rides the cross instead of a
	// plain square, and swaps to its pressed artwork when the stick is
	// clicked (L3/R3). That replaces both the old blue dot and the separate
	// fixed L3/R3 buttons that used to sit above the cross.
	drawPadGlyph(
		ctx,
		PAD_GLYPH_STICK + (clicked ? PAD_GLYPH_PRESSED : 0),
		dotX - PAD_GLYPH_W / 2,
		dotY - PAD_GLYPH_H / 2
	);
}

static void drawPad(RenderContext *ctx, int baseX, int port, const PadState *pad) {
	char line[48];

	snprintf(line, sizeof(line), "PORT %d", port + 1);
	printString(ctx, baseX + 4, 20, 0x808080, line);

	if (!pad->connected) {
		printString(ctx, baseX + 4, 36, 0x505050, "Not connected");
		return;
	}

	const char *typeName = "Digital";
	bool        isAnalog = false;

	if (pad->type == PAD_TYPE_ANALOG_STICK) { typeName = "Analog (green)"; isAnalog = true; }
	if (pad->type == PAD_TYPE_ANALOG)       { typeName = "Analog (red)";   isAnalog = true; }

	printString(ctx, baseX + 60, 20, 0x505050, typeName);

	// Shoulder buttons, top corners. The glyphs carry their own L1/L2/R1/R2
	// lettering, so no text label is drawn alongside them.
	drawPadButton(ctx, baseX +   4, 34, PAD_GLYPH_L1, pad->buttons & PAD_BTN_L1);
	drawPadButton(ctx, baseX +   4, 52, PAD_GLYPH_L2, pad->buttons & PAD_BTN_L2);
	drawPadButton(ctx, baseX + 130, 34, PAD_GLYPH_R1, pad->buttons & PAD_BTN_R1);
	drawPadButton(ctx, baseX + 130, 52, PAD_GLYPH_R2, pad->buttons & PAD_BTN_R2);

	// D-pad cross, left side
	int dpadX = baseX + 36, dpadY = 90;

	drawDpadIcon(ctx, dpadX, dpadY, pad->buttons);

	// Face button diamond, right side
	int faceX = baseX + 116, faceY = 86;

	{
		int hw = PAD_GLYPH_W / 2, hh = PAD_GLYPH_H / 2;

		drawPadButton(ctx, faceX - hw,      faceY - hh - 16,
			PAD_GLYPH_TRIANGLE, pad->buttons & PAD_BTN_TRIANGLE);
		drawPadButton(ctx, faceX - hw + 16, faceY - hh,
			PAD_GLYPH_CIRCLE,   pad->buttons & PAD_BTN_CIRCLE);
		drawPadButton(ctx, faceX - hw,      faceY - hh + 16,
			PAD_GLYPH_CROSS,    pad->buttons & PAD_BTN_CROSS);
		drawPadButton(ctx, faceX - hw - 16, faceY - hh,
			PAD_GLYPH_SQUARE,   pad->buttons & PAD_BTN_SQUARE);
	}

	// Select/Start, bottom center
	drawPadButton(ctx, baseX + 58, 116, PAD_GLYPH_SELECT, pad->buttons & PAD_BTN_SELECT);
	drawPadButton(ctx, baseX + 86, 116, PAD_GLYPH_START,  pad->buttons & PAD_BTN_START);

	// L3/R3 (stick clicks), only meaningful on analog pads but harmless to
	// show regardless
	// Analog sticks. L3/R3 are shown by the stick glyph switching to its
	// pressed artwork, so they need no separate buttons of their own.
	drawAnalogStick(ctx, baseX + 40,  168, pad->leftX,  pad->leftY,  isAnalog,
		pad->buttons & PAD_BTN_L3);
	drawAnalogStick(ctx, baseX + 120, 168, pad->rightX, pad->rightY, isAnalog,
		pad->buttons & PAD_BTN_R3);

	// Live combo readout occupies the old L-stick/R-stick label row. This
	// leaves a clean gap before the footer even when its entries are glyphs.
	char combo[48];
	buildComboString(pad->buttons, combo, sizeof(combo));
	printString(ctx, baseX + 4, 191, HIGHLIGHT_COLOR, combo);
}

/* Menu callback */

void runPadTest(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
) {
	(void) state;
	(void) item;

	static PadState pads[2];

	pads[0].connected  = false;
	pads[1].connected  = false;
	pads[0].configStep = 0;
	pads[1].configStep = 0;
	// buttons is static and survives between calls - if it's not reset too,
	// it still holds whatever was pressed at the moment of the *last* exit
	// (Select+Start), which would immediately re-trigger the exit check
	// below before a single real poll happens this time around. This was
	// the entire "only runs once" bug.
	pads[0].buttons    = 0;
	pads[1].buttons    = 0;

	// Debounce: wait for the button that opened this screen to be released.
	while (pollController(0) | pollController(1))
		;

	for (;;) {
		// Rumble control from last frame's button state, independently on
		// each port: L1+R1 held together = small motor, L2+R2 held
		// together = big motor.
		uint8_t smallMotor0 = ((pads[0].buttons & PAD_BTN_L1) && (pads[0].buttons & PAD_BTN_R1)) ? 0xff : 0x00;
		uint8_t bigMotor0   = ((pads[0].buttons & PAD_BTN_L2) && (pads[0].buttons & PAD_BTN_R2)) ? 0xff : 0x00;
		uint8_t smallMotor1 = ((pads[1].buttons & PAD_BTN_L1) && (pads[1].buttons & PAD_BTN_R1)) ? 0xff : 0x00;
		uint8_t bigMotor1   = ((pads[1].buttons & PAD_BTN_L2) && (pads[1].buttons & PAD_BTN_R2)) ? 0xff : 0x00;

		pollPad(0, &pads[0], smallMotor0, bigMotor0);
		pollPad(1, &pads[1], smallMotor1, bigMotor1);

		if ((pads[0].buttons & PAD_BTN_START) && (pads[0].buttons & PAD_BTN_SELECT)) {
			playCancelSound();
			break;
		}

		beginFrame(ctx);
		drawXMBBackground(ctx);

		printString(ctx, 16, 204, 0x505050,
			CH_PS1_L1_BUTTON "+" CH_PS1_R1_BUTTON ": small motor   "
			CH_PS1_L2_BUTTON "+" CH_PS1_R2_BUTTON ": big motor");
		printString(ctx, 16, 218, 0x505050,
			CH_PS1_SELECT_BUTTON "+" CH_PS1_START_BUTTON
			" Return to menu");

		drawPad(ctx, 0,          0, &pads[0]);
		drawPad(ctx, HALF_WIDTH, 1, &pads[1]);

		endFrame(ctx);
	}

	// Make sure the motors are off on both ports before leaving.
	pollPad(0, &pads[0], 0x00, 0x00);
	pollPad(1, &pads[1], 0x00, 0x00);

	// Flush button state before handing control back to the outer menu
	// system - see the matching comment in memcard.c's
	// runMemoryCardManager() for the full explanation. Without this,
	// whichever button you exited with is still held when control
	// returns, and the outer menu's stale lastButtons misreads it as a
	// fresh "confirm" press on this same still-highlighted item,
	// immediately re-opening this screen.
	while (pollController(0) | pollController(1))
		;

	// Deliberately no enterMainMenu() call here - state->currentMenu and
	// state->menuCursor were never touched anywhere in this function
	// (this screen renders through its own self-contained loop
	// instead), so they still correctly point at whichever item was
	// selected to get here.
}
