/*
 * PSX-iTests - shared "hand the machine over" helpers (see handoff.h for the
 * full explanation of the stuck-DMA bug this exists to fix).
 */

#include <stdint.h>
#include "ps1/cache.h"
#include "ps1/cop0.h"
#include "ps1/gpucmd.h"
#include "ps1/registers.h"
#include "main/handoff.h"
#include "main/tty_serial.h"

// Longest we will wait for an in-flight DMA to drain before forcing it to
// stop. A full display list is a few thousand words and completes in well
// under a millisecond, so this only exists so a channel that is already
// wedged can never hang the hand-off itself.
#define DMA_DRAIN_TIMEOUT 0x100000u

static void quiesceCommon(void) {
	// 0. Take our TTY device back out of the kernel's device table, FIRST,
	//    while the BIOS is still usable and interrupts are still on.
	//
	//    ttyDCB and its handlers live inside this image at ~0x8001xxxx. A
	//    program that loads over that range leaves the kernel's device table
	//    pointing into the middle of the newly loaded program, and the next
	//    BIOS call that touches "tty" jumps into it. That is invisible until
	//    something actually loads low - which is exactly the difference
	//    between the targets that have always worked here (cdloader at
	//    0x801EA300, the SIO loader at 0x801B0000, UniROM at 0x801D0000, all
	//    of which leave this image intact) and the 240p Test Suite, which
	//    loads at 0x80010000.
	uninstallSerialTTY();

	// 1. Silence the SPU. UniROM never touches the SPU at all (there is not
	//    a single access to the 0x1f801c00 block anywhere in its binary), so
	//    without this our background music would carry on playing underneath
	//    it forever.
	SPU_KOFF0 = 0xffff;
	SPU_KOFF1 = 0xffff;
	SPU_MVOLL = 0;
	SPU_MVOLR = 0;

	// 2. Let the display-list DMA started by the last endFrame() finish on
	//    its own, then force-stop every channel by clearing its CHCR. This
	//    is the actual fix: clearing CHCR is what releases the GPU's
	//    "DMA in progress" state. Zeroing DMA_DPCR (what the old launchers
	//    did) does not - it only removes the master enable and strands the
	//    channel mid-chain.
	unsigned timeout = DMA_DRAIN_TIMEOUT;
	while ((DMA_CHCR(DMA_GPU) & DMA_CHCR_ENABLE) && --timeout)
		__asm__ volatile("");

	for (int ch = 0; ch <= 6; ch++)
		DMA_CHCR(ch) = 0;

	// 3. Software-reset the GPU (GP1(00)). This clears the GP0 command FIFO,
	//    so a primitive that was only half-consumed when we cut the DMA off
	//    cannot corrupt the first commands the next program sends. Now that
	//    step 2 has released the channel, GPUSTAT bit 26 comes back and
	//    UniROM's wait loop at 0x801DB40C can complete.
	GPU_GP1 = gp1_resetGPU();

	// 4. (Interrupt masking is not done here - it differs between the two
	//    hand-off styles, see quiesceForHandoff() and quiesceForBIOSExec()
	//    below.)

	// 5. Clear any COP0 hardware breakpoint still armed. This is the actual
	//    cause of "UniROM launches and the console just restarts": Settings
	//    -> Reboot's fast-reboot path (common/reboot.c: softFastReboot() /
	//    softFastRebootWithConfig()) arms a COP0 data breakpoint over
	//    0x80030000-0x8003ffff to protect its dummy shell, and reboot.c's own
	//    prepareForReboot() has to explicitly clear DCIC/BDA/BDAM before its
	//    own jump for exactly this reason - its comment there spells out
	//    that a stale breakpoint survives a BIOS soft-reset (COP0 isn't
	//    touched by it) and makes the *next* thing that writes into that RAM
	//    range trap. That "next thing" was never a launched EXE before now,
	//    only ever another reboot - so this hand-off path never carried the
	//    same fix. UniROM's own start-up zero-fills a chunk of its BSS
	//    (0x801D0000's very first instructions), and if Fast Reboot had
	//    armed and left this breakpoint any time earlier in the same
	//    power-on session, it is UniROM's write into RAM - not anything
	//    about UniROM itself - that trips it and resets the console.
	//    Harmless to clear even when nothing was ever armed.
	cop0_setReg(COP0_DCIC, 0);
	cop0_setReg(COP0_BDA,  0);
	cop0_setReg(COP0_BDAM, 0);

	// Note we deliberately leave DMA_DPCR alone - see handoff.h.
}

void quiesceForHandoff(void) {
	quiesceCommon();

	// Mask every interrupt source and acknowledge anything already pending
	// (I_STAT bits are cleared by writing a 0 to them), then turn interrupts
	// off at the CPU.
	//
	// Correct for a bare-metal target, which installs its own handlers
	// before enabling anything. Wrong for a target handed over through
	// Exec(): that one expects the interrupt state the BIOS would have given
	// it, and if it waits on a kernel event the event can never arrive.
	IRQ_MASK = 0;
	IRQ_STAT = 0;
	__asm__ volatile("mtc0 $zero, $12\n");
}

void quiesceForBIOSExec(void) {
	quiesceCommon();

	/*
	 * Deliberately nothing further: the interrupt mask, I_STAT and COP0 SR
	 * are left exactly as they are, so Exec() and the program it starts see
	 * the machine in the state the BIOS would have handed them.
	 *
	 * A still-pending interrupt therefore fires into the dashboard's own
	 * handler, which is harmless - this path never erases the dashboard, so
	 * that handler is still resident and still valid right up until the
	 * launched program installs its own.
	 */
}

void quiesceForFirmwareReset(void) {
	quiesceForHandoff();
}

__attribute__((noreturn)) void jumpToLoadedEXE(uint32_t pc, uint32_t gp, uint32_t sp) {
	// The program's code was just written into RAM as data; invalidate the
	// instruction cache so the CPU cannot execute stale lines from whatever
	// occupied those addresses before.
	flushCache();

	// Nothing may touch the C stack after $sp is reassigned, so the register
	// setup and the jump have to be a single asm block.
	__asm__ volatile(
		"move $gp, %1\n"
		"move $sp, %2\n"
		"jr   %0\n"
		"nop\n"
		:
		: "r"(pc), "r"(gp), "r"(sp)
		: "memory"
	);

	__builtin_unreachable();
}

__attribute__((noreturn)) void launchPSEXEImage(const uint8_t *exe) {
	const PSEXEHeader *hdr = (const PSEXEHeader *) exe;
	const uint32_t    *src = (const uint32_t *)(exe + PSEXE_PAYLOAD_OFFSET);

	uint32_t entry = hdr->pc;
	uint32_t gp    = hdr->gp;
	// The BIOS computes the initial stack pointer as spBase + spOffset; fall
	// back to just below the top of main RAM if the header leaves it unset.
	uint32_t sp    = hdr->spBase ? (hdr->spBase + hdr->spOffset) : 0x801ff000;
	uint32_t dst   = hdr->textAddr;
	uint32_t words = (hdr->textSize + 3) / 4;

	quiesceForHandoff();

	volatile uint32_t *d = (volatile uint32_t *) dst;

	// memmove semantics. Embedded blobs live in .rodata, which today sits
	// comfortably below every load address we use - but .rodata grows every
	// time an asset is added, and a forward copy would silently eat its own
	// source the moment it did overlap. Copying downwards when the source is
	// below the destination costs nothing and removes that trap entirely.
	if ((const uint32_t *) dst > src) {
		for (uint32_t i = words; i-- > 0; )
			d[i] = src[i];
	} else {
		for (uint32_t i = 0; i < words; i++)
			d[i] = src[i];
	}

	jumpToLoadedEXE(entry, gp, sp);
}
