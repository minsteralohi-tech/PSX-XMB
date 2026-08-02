/*
 * PSX-iTests - disc game ID detection (see gameid.c)
 *
 * Watches the CD lid and, when a disc is inserted, reads SYSTEM.CNF to find
 * the boot executable name - the same string jdfr228's PS1-Disc-Based-Game-ID
 * BIOS patch reports, e.g. "SLUS_004.02". The dashboard shows it briefly in
 * the top-right corner.
 */

#pragma once

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
	GAMEID_FOUND,      /* id[] holds a boot name                 */
	GAMEID_NO_DISC,    /* disc unreadable or not ISO9660         */
	GAMEID_UNKNOWN     /* readable, but no usable BOOT= line     */
} GameIdResult;

typedef struct {
	GameIdResult state;
	char         id[32];
	int          noticeTime;   /* frames left to show the notification */
} GameIdState;

void gameIdInit(void);

/*
 * Call once per frame from the main loop, with a 2048-byte scratch buffer.
 *
 * Cheap almost always: it polls the drive's shell flag a few times a second
 * and does nothing else. On a lid-open -> lid-closed transition it waits for
 * the drive to settle and then performs a blocking disc read of a few hundred
 * milliseconds. Every wait inside is bounded, so a missing or faulty drive
 * degrades to "no ID" rather than freezing the dashboard.
 */
void gameIdPoll(uint8_t *scratch, int scratchSize);

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
