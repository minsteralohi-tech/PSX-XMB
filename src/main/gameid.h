/*
 * PSX-iTests - disc game ID detection (see gameid.c)
 *
 * Watches the CD lid and, when a disc is inserted, reads SYSTEM.CNF to find
 * the boot executable name - the same string jdfr228's PS1-Disc-Based-Game-ID
 * BIOS patch reports, e.g. "SLUS_004.02". The dashboard shows it briefly in
 * the top-right corner.
 */

#pragma once

/*
 * DISABLED BY DEFAULT - the dashboard must boot.
 *
 * Set to 1 to compile the disc scan back in. Left off because every version
 * tried so far has crashed on real hardware, and a dashboard that does not
 * start is far worse than one without game names. See the analysis at the top
 * of gameid.c before turning this on.
 *
 * The database, the lookup and the notification card are all finished and
 * correct; the unsolved part is purely how to read SYSTEM.CNF from inside a
 * running bare-metal program.
 */
#define GAMEID_ENABLED 0

#include <stdint.h>

#include "main/renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How long the corner notification stays up, in frames (~4s at 60Hz). */
#define GAMEID_NOTICE_FRAMES 240

/* Scratch buffer the caller must provide: one ISO9660 sector. */
#define GAMEID_SCRATCH_SIZE 2048

typedef enum {
	GAMEID_IDLE = 0,   /* nothing read yet, or the lid is open   */
	GAMEID_READING,    /* disc spun up, blocking read in progress */
	GAMEID_FOUND,      /* id[] holds a boot name                 */
	GAMEID_NO_DISC,    /* disc unreadable or not ISO9660         */
	GAMEID_UNKNOWN,    /* readable, but no usable BOOT= line     */
	GAMEID_UNLISTED    /* boot name read, but not in the table    */
} GameIdResult;

typedef struct {
	GameIdResult state;
	char         id[64];   /* game name, or the boot name as a fallback */
	int          noticeTime;   /* frames left to show the notification */
} GameIdState;

void gameIdInit(void);

/*
 * Call once per frame from the main loop, with a scratch buffer of at least
 * GAMEID_SCRATCH_SIZE bytes.
 *
 * Does nothing on almost every frame. It performs one disc scan a few seconds
 * after boot, and thereafter only when gameIdRequestScan() has been called.
 *
 * There is NO automatic lid detection - see the note at the top of gameid.c.
 * Watching the shell-open flag means issuing CD-ROM commands behind the live
 * BIOS interrupt handler's back, which crashed the dashboard on both hardware
 * and emulator.
 */
void gameIdPoll(uint8_t *scratch, int scratchSize);

/*
 * Ask for a disc scan on the next frame. Costs a visible pause of a few
 * hundred milliseconds, announced on screen while it happens, so call it in
 * response to something the user did rather than on a timer.
 */
void gameIdRequestScan(void);

const GameIdState *gameIdGet(void);

/*
 * Draw the corner notification if one is pending. Call last in the frame, so
 * it sits over whatever screen is up. Does nothing when idle.
 */
void drawGameIdNotice(RenderContext *ctx);

/* Dismiss the corner notification early. */
void gameIdClearNotice(void);

#ifdef __cplusplus
}
#endif
