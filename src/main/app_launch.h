/*
 * PSX-iTests - generic two-stage launcher for embedded standalone PS-EXEs
 *
 * WHAT THIS REPLACES
 * ------------------
 * The dashboard used to contain a whole serial receiver (sio_loader.c) plus
 * its own staged trampoline pinned at 0x8000c800. All of that is gone. The
 * SIO loader is now an ordinary standalone PS-EXE embedded as an asset, and
 * this module is the generic machinery for handing the console over to it -
 * or to UniROM, a 240p test suite build, or anything else added later.
 *
 * WHY TWO STAGES
 * --------------
 * The dashboard is enormous. Its PS-EXE payload alone is ~1.59 MB, so it
 * occupies roughly 0x80010000-0x8019c800 before .bss, and .bss pushes
 * _imageEnd higher still. Almost any program worth launching wants RAM the
 * dashboard is sitting in - the standalone SIO loader wants 0x801b0000, and
 * its staging area is the whole of 0x80010000-0x801b0000.
 *
 * A single-stage launcher (the old launchPSEXEImage()) copies the payload
 * from C code that is itself executing out of the region being overwritten.
 * That works today only because the embedded blobs happen to sit below every
 * load address in use, and it offers no way to clear the dashboard out of RAM
 * at all - the code doing the clearing would be the first casualty.
 *
 * So:
 *
 *   stage 0   the dashboard: pick an arena, validate, quiesce, install stage 1
 *   stage 1   a ~200-byte position-independent blob running from the arena,
 *             which is outside both the source and the destination. It copies
 *             the payload, zero-fills whatever RAM was asked for (including
 *             the dashboard itself), fills the target's BSS, flushes the
 *             instruction cache, sets $gp/$sp and jumps.
 *   stage 2   the launched program
 *
 * Nothing returns to C after stage 1 starts.
 *
 * WHEN STAGE 1 IS ACTUALLY NEEDED
 * -------------------------------
 * Most of the time it is not, and using it anyway is a bad trade. Copying the
 * payload straight from C and jumping - handoff.c's launchPSEXEImage(), the
 * path Fast Boot and Tools -> UniROM 8.0 already use - is safe whenever the
 * destination and the memfill are clear of the code doing the copying and of
 * its stack. That is true for the standalone SIO loader (0x801b0000), for
 * UniROM (0x801d0000) and for anything else that loads above the dashboard's
 * live region, and it is the only handoff on this console with a track record
 * of working.
 *
 * So the planner picks:
 *
 *   direct    the destination and every zero-fill clear the dashboard's
 *             .text and its stack. No trampoline, no arena.
 *   stage 1   they do not - the payload lands on the copier, or RAM is being
 *             erased out from under it - so the copy has to be performed by
 *             code running somewhere neither side can reach.
 *
 * "Live" here means .text plus the current stack, not the whole image: .bss,
 * the heap and everything else above the stack is expendable the instant
 * quiesceForHandoff() has run, because nothing reads it again.
 *
 * PICKING THE ARENA
 * -----------------
 * There is no single address that is free for every target, which is what
 * made the previous fixed 0x8000c800 choice fragile:
 *
 *   standalone SIO loader   0x801b0000-0x801c0000
 *   UniROM 8.0.K            0x801d0000-0x801f1000
 *   cdloader.exe            0x801ea300-0x80200000
 *   ordinary homebrew       0x80010000 upwards
 *
 * So the arena is chosen at run time from a candidate list, taking the first
 * one that provably clears the destination, the source blob and the target's
 * BSS. The candidates are all above the dashboard's own _imageEnd, so the
 * install cannot corrupt a global the dashboard is still reading, with BIOS
 * kernel scratch as a last resort for targets that want the whole top of RAM.
 *
 * The chosen arena and the full plan are exposed through planEmbeddedApp() so
 * the UI can show them before committing - if a launch ever misbehaves again,
 * the addresses involved are on screen rather than guessed at.
 */

#pragma once

#include <stdint.h>
#include "main/handoff.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Lowest address a launched program may occupy; below this is BIOS/kernel. */
#define APP_RAM_BASE 0x80010000u
#define APP_RAM_TOP  0x80200000u

/* Arena: 4 KB, stage 1 code at +0, its parameter block at +0x800, and the
 * zero-fill descriptor list at +0x900. */
#define APP_ARENA_SIZE        0x1000u
#define APP_ARENA_PARAM_OFF   0x800u
#define APP_ARENA_FILL_OFF    0x900u
#define APP_STUB_MAX_SIZE     APP_ARENA_PARAM_OFF

/* [0x80010000, dest), up to two pieces of [destEnd, 0x80200000) once the
 * arena has been punched out of it, and the target's BSS. */
#define APP_MAX_FILLS 6

typedef struct {
	int      useBiosExec;      /* hand over through BIOS Exec() instead    */
	int      useStage1;        /* 0 = direct copy-and-jump from C          */
	uint32_t entry, gp, sp;
	uint32_t dest, destEnd, bodySize;
	uint32_t src;              /* payload inside the dashboard's .rodata  */
	uint32_t arena;            /* where stage 1 will run                  */
	uint32_t imageEnd;         /* dashboard's _imageEnd, for display      */
	uint32_t liveEnd;          /* end of what the dashboard is still using */
	uint32_t fillCount;
	uint32_t fillStart[APP_MAX_FILLS];
	uint32_t fillBytes[APP_MAX_FILLS];
} AppLaunchPlan;

typedef enum {
	APP_PLAN_OK = 0,
	APP_PLAN_BAD_MAGIC,      /* not a PS-EXE                             */
	APP_PLAN_BAD_SIZE,       /* zero or absurd payload size              */
	APP_PLAN_UNALIGNED,      /* dest/entry/sp/bss not word aligned       */
	APP_PLAN_DEST_RANGE,     /* destination outside usable RAM, or wraps  */
	APP_PLAN_ENTRY_RANGE,    /* entry point outside the loaded image      */
	APP_PLAN_SP_RANGE,       /* initial SP outside usable RAM            */
	APP_PLAN_BSS_RANGE,      /* memfill range outside usable RAM         */
	APP_PLAN_NO_ARENA,       /* no candidate clears dest, source and BSS */
	APP_PLAN_TOO_MANY_FILLS  /* internal: fill list overflowed           */
} AppPlanResult;

/*
 * Work out everything about a launch without performing any of it. Safe to
 * call from menu code; touches no hardware and never returns anything but a
 * fully populated plan on APP_PLAN_OK.
 *
 * eraseRam requests that all of main RAM outside the target, the arena and
 * the BIOS's own first 64 KB be zeroed before the jump - i.e. the dashboard
 * is wiped out. Programs that assume a cold-boot RAM state need it; ordinary
 * PS-EXEs do not care, and the BIOS itself does not do it.
 */
AppPlanResult planEmbeddedApp(const uint8_t *exe, int eraseRam,
                              AppLaunchPlan *plan);

/*
 * Ask for the BIOS Exec() hand-off instead of the direct jump.
 *
 * Exec() is how the BIOS itself starts a PS-EXE: given the header from offset
 * 0x10 onwards it sets $gp, builds the stack and frame pointer, zero-fills the
 * BSS and calls the entry point, with the kernel live and interrupts left as
 * the BIOS would have left them. Targets that lean on BIOS services from their
 * first instruction - UniROM installs its own kernel exception handler and TTY
 * redirect - can start correctly this way and not from a hand-rolled jump.
 *
 * It cannot be combined with erasing RAM or with the stage 1 trampoline: Exec()
 * runs kernel code and returns into the caller's world on failure, so the
 * dashboard has to still be there. Returns 0 and leaves the plan unchanged if
 * the two are incompatible.
 */
int planUseBiosExec(AppLaunchPlan *plan, int enable);

/* Human-readable form of an AppPlanResult, for the UI. */
const char *appPlanResultText(AppPlanResult result);

/* Execute a plan produced by planEmbeddedApp(). Quiesces the hardware,
 * installs stage 1 into the arena and jumps into it. Never returns. */
__attribute__((noreturn)) void runAppLaunch(const AppLaunchPlan *plan);

/*
 * Plan and launch in one call. Returns only on failure, with the reason - so
 * a caller can show an error instead of a black screen.
 */
AppPlanResult launchEmbeddedApp(const uint8_t *exe, int eraseRam);

/* Stage 1, from app_stub.s. */
extern const uint32_t appStubStart[];
extern const uint32_t appStubEnd[];

#ifdef __cplusplus
}
#endif
