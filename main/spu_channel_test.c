/*
 * PSX-iTests - SPU channel test
 *
 * Automatically cycles through all 24 SPU channels in sequence, playing a
 * short test tone on each one in turn, to confirm every channel actually
 * outputs sound - no manual stepping required, just watch/listen and let
 * it run. Since channels 0-2 are normally used for the scroll/confirm/BGM
 * sounds, testing them interrupts that playback - normal audio is restored
 * on exit the same way the SPU RAM test restores it. Loops back to channel
 * 0 after the last channel and keeps going until any button is pressed.
 */

#include <stdint.h>
#include <stdio.h>
#include "common/sio0.h"
#include "common/spu.h"
#include "main/font.h"
#include "main/mainmenu.h"
#include "main/xmb_bg.h"
#include "main/sound.h"
#include "main/spu_channel_test.h"

// How many frames to hold on each channel before advancing to the next one.
// At 60 fps this is 0.75 s, long enough to clearly hear the ~0.16 s test
// tone with a bit of a gap before the next channel starts.
#define FRAMES_PER_CHANNEL 45

void runSPUChannelTest(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
) {
	(void) state;
	(void) item;

	int channel      = 0;
	int frameInStep  = 0;

	// Remember whether the user had background music on before this test.
	// The test plays its tone on every channel including channel 3 (the BGM
	// channel), so we have to rebuild BGM afterwards - but if the user had
	// muted the music, we must restore it muted rather than turning it back
	// on behind their back.
	bool bgmWasOn = isBGMEnabled();

	// Debounce: wait for the button that opened this screen to be released.
	while (pollController(0) | pollController(1))
		;

	playTestTone(channel);

	for (;;) {
		if (pollController(0) | pollController(1))
			break;

		frameInStep++;
		if (frameInStep >= FRAMES_PER_CHANNEL) {
			frameInStep = 0;
			channel     = (channel + 1) % SPU_NUM_CHANNELS;
			playTestTone(channel);
		}

		char line[64];

		beginFrame(ctx);
		drawXMBBackground(ctx);

		printString(ctx, 16, 20, 0x808080, "SPU CHANNEL TEST");

		snprintf(
			line, sizeof(line), "Channel: %d / %d", channel + 1,
			SPU_NUM_CHANNELS
		);
		printString(ctx, 16, 60, 0xffffff, line);

		printString(ctx, 16,  90, 0x808080, "Auto-cycling through all channels...");
		printString(ctx, 16, 102, 0x808080, "Any button: return to menu");

		endFrame(ctx);
	}

	// Restore normal scroll/confirm/cancel/BGM playback on channels 0-3.
	initSound();
	playBGM();               // re-keys the BGM loop on channel 3 (audible)
	if (!bgmWasOn)
		toggleBGM();         // ...but the user had music off, so re-mute it

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
