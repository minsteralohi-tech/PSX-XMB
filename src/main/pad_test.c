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
	{ PAD_BTN_UP,       "U"                     },
	{ PAD_BTN_DOWN,     "D"                     },
	{ PAD_BTN_LEFT,     "L"                     },
	{ PAD_BTN_RIGHT,    "R"                     },
	{ PAD_BTN_L1,       "L1"                    },
	{ PAD_BTN_L2,       "L2"                    },
	{ PAD_BTN_L3,       "L3"                    },
	{ PAD_BTN_R1,       "R1"                    },
	{ PAD_BTN_R2,       "R2"                    },
	{ PAD_BTN_R3,       "R3"                    },
	{ PAD_BTN_SELECT,   "SEL"                   },
	{ PAD_BTN_START,    "STA"                   },
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

	if (first)
		snprintf(out, outSize, "-");
}

/* Drawing */

#define HALF_WIDTH 160

static void drawButtonGlyph(
	RenderContext *ctx,
	int           x,
	int           y,
	int           boxSize,
	bool          pressed,
	const char    *glyph
) {
	if (pressed)
		drawRect(ctx, x - 2, y - 2, boxSize, boxSize, HIGHLIGHT_COLOR, false);

	printString(ctx, x, y, pressed ? 0x000000 : 0x808080, glyph);
}

static void drawLabeledButton(
	RenderContext *ctx,
	int           x,
	int           y,
	int           w,
	bool          pressed,
	const char    *label
) {
	drawRect(ctx, x, y, w, 12, pressed ? HIGHLIGHT_COLOR : 0x242424, false);
	printString(ctx, x + 3, y + 2, pressed ? 0x000000 : 0x808080, label);
}

// D-pad: back to the original, simplest version from the very first pad
// tester build - plain U/D/L/R text labels via drawLabeledButton, the
// same function already used successfully for L1/L2/R1/R2/L3/R3
// throughout this whole file. After several attempts at a more elaborate
// shape (triangles, a rect+triangle "house" shape, a texture-based icon),
// none of them landed - this reverts back to the simple version that
// actually worked, rather than continuing to iterate on a look that
// hasn't come together. The offset (14px) and box size (14px) here are
// the original values; only the D-pad's overall center position
// (centerX, centerY, set by the caller) reflects the layout spacing
// refined since then for the rest of the pad tester.
static void drawDpadIcon(
	RenderContext *ctx,
	int           centerX,
	int           centerY,
	uint16_t      buttons
) {
	drawLabeledButton(ctx, centerX,      centerY - 14, 14, buttons & PAD_BTN_UP,    "U");
	drawLabeledButton(ctx, centerX,      centerY + 14, 14, buttons & PAD_BTN_DOWN,  "D");
	drawLabeledButton(ctx, centerX - 14, centerY,      14, buttons & PAD_BTN_LEFT,  "L");
	drawLabeledButton(ctx, centerX + 14, centerY,      14, buttons & PAD_BTN_RIGHT, "R");
}

static void drawAnalogStick(
	RenderContext *ctx,
	int           centerX,
	int           centerY,
	uint8_t       rawX,
	uint8_t       rawY,
	bool          active
) {
	const int radius = 16;

	drawRect(ctx, centerX - radius, centerY - 1, radius * 2, 2, 0x383838, false);
	drawRect(ctx, centerX - 1, centerY - radius, 2, radius * 2, 0x383838, false);

	int dotX = centerX;
	int dotY = centerY;

	if (active) {
		dotX += ((int) rawX - 128) * radius / 128;
		dotY += ((int) rawY - 128) * radius / 128;
	}

	drawRect(ctx, dotX - 3, dotY - 3, 6, 6, active ? HIGHLIGHT_COLOR : 0x505050, false);
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

	// Shoulder buttons, top corners
	drawLabeledButton(ctx, baseX +  4, 36, 24, pad->buttons & PAD_BTN_L1, "L1");
	drawLabeledButton(ctx, baseX +  4, 50, 24, pad->buttons & PAD_BTN_L2, "L2");
	drawLabeledButton(ctx, baseX + 132, 36, 24, pad->buttons & PAD_BTN_R1, "R1");
	drawLabeledButton(ctx, baseX + 132, 50, 24, pad->buttons & PAD_BTN_R2, "R2");

	// D-pad cross, left side
	int dpadX = baseX + 36, dpadY = 90;

	drawDpadIcon(ctx, dpadX, dpadY, pad->buttons);

	// Face button diamond, right side
	int faceX = baseX + 116, faceY = 86;

	drawButtonGlyph(ctx, faceX,      faceY - 16, 14, pad->buttons & PAD_BTN_TRIANGLE, CH_PS1_TRIANGLE_BUTTON);
	drawButtonGlyph(ctx, faceX + 16, faceY,      14, pad->buttons & PAD_BTN_CIRCLE,   CH_PS1_CIRCLE_BUTTON);
	drawButtonGlyph(ctx, faceX,      faceY + 16, 14, pad->buttons & PAD_BTN_CROSS,    CH_PS1_CROSS_BUTTON);
	drawButtonGlyph(ctx, faceX - 16, faceY,      14, pad->buttons & PAD_BTN_SQUARE,   CH_PS1_SQUARE_BUTTON);

	// Select/Start, bottom center
	drawButtonGlyph(ctx, baseX + 60, 119, 20, pad->buttons & PAD_BTN_SELECT, CH_PS1_SELECT_BUTTON);
	drawButtonGlyph(ctx, baseX + 90, 119, 20, pad->buttons & PAD_BTN_START,  CH_PS1_START_BUTTON);

	// L3/R3 (stick clicks), only meaningful on analog pads but harmless to
	// show regardless
	drawLabeledButton(ctx, baseX + 20,  142, 24, pad->buttons & PAD_BTN_L3, "L3");
	drawLabeledButton(ctx, baseX + 116, 142, 24, pad->buttons & PAD_BTN_R3, "R3");

	// Analog sticks
	drawAnalogStick(ctx, baseX + 40,  174, pad->leftX,  pad->leftY,  isAnalog);
	drawAnalogStick(ctx, baseX + 120, 174, pad->rightX, pad->rightY, isAnalog);

	printString(ctx, baseX + 4, 193, 0x505050, "L stick");
	printString(ctx, baseX + 96, 193, 0x505050, "R stick");

	// Live combo readout, e.g. "X+O+R1"
	char combo[48];
	buildComboString(pad->buttons, combo, sizeof(combo));
	printString(ctx, baseX + 4, 202, HIGHLIGHT_COLOR, combo);
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

		printString(ctx, 16, 210, 0x505050, "L1+R1: small motor   L2+R2: big motor (both ports)");
		printString(ctx, 16, 222, 0x505050, "PORT 1 START+SELECT: return to menu");

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
