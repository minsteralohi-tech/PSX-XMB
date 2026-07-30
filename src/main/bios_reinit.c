/*
 * PSX BIOS warm-boot reconstruction for executable handoff.
 *
 * Adapted from the source of the supplied, working cdloader.exe:
 * Ps1testloader-baremetalsdk-main/src/bios.c.
 *
 * This deliberately reconstructs the kernel tables in place.  Calling
 * close()/RemoveDevice() on the old tables is unsafe here: a UniROM cartridge
 * can own or patch those tables, and our own replacement tty contains
 * callbacks into the dashboard image that is about to be erased.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "main/bios_reinit.h"

#define BIOS_DEFAULT_EVCB     0x10u
#define BIOS_DEFAULT_TCB      0x04u
#define BIOS_DEFAULT_STACKTOP ((void *) 0x801fff00u)

typedef void (*VoidFunction)(void);

extern void biosEnterCriticalSection(void);
extern void biosExitCriticalSection(void);
extern void biosInitVectors(void);
extern void biosSetDefaultExitFromException(void);
extern void biosInstallExceptionHandlers(void);
extern void biosInstallDevices(uint32_t enableTTY);
extern void biosAdjustA0Table(void);
extern void biosSetConf(uint32_t evcb, uint32_t tcb, void *stackTop);
extern void biosSetMemSize(uint32_t megabytes);
extern void biosFakeEnqueueCdIntr(void);

/* Match cdloader exactly: the A0 dispatch table is at physical 0x200. */
static volatile VoidFunction *const biosA0Table =
	(volatile VoidFunction *) 0x00000200u;

static VoidFunction parseWarmBootCall(unsigned instructionIndex) {
	const uint32_t *warmBoot = (const uint32_t *) biosA0Table[0xa0];
	uint32_t instruction = warmBoot[instructionIndex];
	uint32_t target = ((uint32_t) warmBoot & 0xf0000000u) |
		((instruction & 0x03ffffffu) << 2);
	return (VoidFunction) target;
}

void reinitializeBIOSForHandoff(void) {
	biosEnterCriticalSection();

	/*
	 * cdloader clears the complete BIOS allocation area before rebuilding
	 * it.  This removes cartridge hooks and the dashboard's tty DCB/FCBs
	 * without invoking either one's close callbacks.
	 */
	memset((void *) 0xa000e000u, 0, 0x2000u);

	/*
	 * WarmBoot instructions 12 and 14 call the ROM routines which restore
	 * the relocated kernel at 0x500 and the A0 table at 0x200.  This is how
	 * the supplied cdloader remains portable across retail BIOS revisions
	 * instead of hard-coding ROM addresses.
	 */
	parseWarmBootCall(12)();
	parseWarmBootCall(14)();

	biosInitVectors();
	biosAdjustA0Table();
	biosInstallExceptionHandlers();
	biosSetDefaultExitFromException();

	*(volatile uint16_t *) 0xbf801070u = 0;
	*(volatile uint16_t *) 0xbf801074u = 0;

	/* Install the retail dummy tty plus the standard CD-ROM/card devices. */
	biosInstallDevices(0);

	/*
	 * SetConf normally re-enqueues the CD subsystem, which is both needless
	 * and capable of duplicating CD events here.  Match cdloader: temporarily
	 * replace A0(A2h) with a tiny early-return shim while SetConf runs.
	 */
	VoidFunction realEnqueueCdIntr = biosA0Table[0xa2];
	biosA0Table[0xa2] = biosFakeEnqueueCdIntr;
	biosSetConf(BIOS_DEFAULT_EVCB, BIOS_DEFAULT_TCB, BIOS_DEFAULT_STACKTOP);
	biosA0Table[0xa2] = realEnqueueCdIntr;

	/* Match the power-on BIOS convention used by the working loader. */
	biosSetMemSize(8);
	biosExitCriticalSection();
}
