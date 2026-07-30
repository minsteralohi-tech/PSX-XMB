/*
 * PSX-iTests - shared "hand the machine over" helpers
 *
 * Every path that chain-loads another program (Fast Boot's cdloader.exe,
 * Tools -> UniROM 8.0, and Settings -> SIO Loader) needs the same
 * thing first: put the hardware back into a state a freshly-started PS-EXE
 * can cope with. This module is that step, in one place.
 *
 * WHY THIS EXISTS (hardware state and BIOS state both outlive the caller):
 *
 * renderer.c's endFrame() finishes with sendGPULinkedList() and returns
 * immediately - it does NOT wait for the transfer to finish. Waiting is
 * deferred to the *next* frame's send. So at the instant a menu action runs,
 * DMA channel 2 is still walking our display list.
 *
 * The old launchers' first act was `DMA_DPCR = 0`, yanking the master enable
 * out from under that live transfer. That leaves channel 2's CHCR "busy" bit
 * (1 << 24) set forever, mid-chain, and the GPU still believing a DMA is in
 * progress.
 *
 * cdloader.exe survives this by pure luck of its own start-up: it's a
 * ps1-bare-metal program, so its main() calls setupGPU(), which ends with
 *
 *     DMA_DPCR         |= DMA_DPCR_CH_ENABLE(DMA_GPU);
 *     DMA_CHCR(DMA_GPU) = 0;          // <- clears the stuck channel
 *
 * UniROM does no such thing. Its GPU init (0x801DB968 in the supplied
 * unirom_bin.exe) issues a GP1(00) software reset - which resets the *GPU*
 * but not the *DMA controller* - and then spins at 0x801DB40C:
 *
 *     801DB40C: lw   $t0, 0x1814($v1)      ; GPUSTAT
 *     801DB414: and  $t0, $t0, 0x04000000  ; bit 26, "ready for command word"
 *     801DB418: beq  $t0, $zero, 0x801DB40C
 *
 * With channel 2 still latched busy, that bit may never come back and UniROM
 * can spin there forever. Stopping each CHCR closes that hardware-state gap.
 *
 * The dashboard-resident SIO receiver that used to live here is gone. Serial
 * loading is now a standalone PS-EXE launched through app_launch.c's
 * two-stage handoff, so nothing in this file needs to survive its own
 * payload any more. quiesceForHandoff() is still shared by every path.
 *
 * The handoff deliberately does not reconstruct BIOS tables or traverse
 * inherited device callbacks; that experimental path broke both serial
 * launch and Fast Boot. It also does NOT zero DMA_DPCR: stopping each
 * channel through CHCR is safer and closer to BIOS launch state.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Put the hardware into a state a freshly-started PS-EXE can take over:
 * silence the SPU, drain and stop every DMA channel, software-reset the GPU,
 * mask and acknowledge all interrupts, clear any stale COP0 breakpoint, and
 * disable interrupts at the CPU.
 */
void quiesceForHandoff(void);

/*
 * Hardware-only cleanup for returning to firmware through the BIOS reset
 * vector. Unlike an embedded EXE handoff, this deliberately preserves a
 * physical cartridge's resident BIOS hooks.
 */
void quiesceForFirmwareReset(void);

/*
 * Directly copies an embedded PS-EXE payload to its load address with
 * memmove semantics, flushes the instruction cache, and jumps with the
 * PS-EXE PC/GP/SP register contract. Calls quiesceForHandoff() first.
 * This is intentionally the stable Fast Boot path.
 *
 * The copy has memmove semantics, so it stays correct even if this app's own
 * .rodata (where embedded blobs live) overlaps the target's load address.
 *
 * Fast Boot and embedded UniROM intentionally use this exact stable path.
 */
__attribute__((noreturn)) void launchPSEXEImage(const uint8_t *exe);

/*
 * Flush the instruction cache, then set $gp/$sp and jump to a program that
 * has already been copied into place. Never returns.
 *
 * Defined in handoff.c and used by launchPSEXEImage() there and by
 * app_launch.c's direct handoff, so it has to be declared here - it was
 * previously only ever called within handoff.c and so had no prototype.
 */
__attribute__((noreturn)) void jumpToLoadedEXE(
	uint32_t pc,
	uint32_t gp,
	uint32_t sp
);

/* Standard PS-EXE header: the first 2048 bytes of any .exe file. */
typedef struct {
	char     magic[8];      // "PS-X EXE"
	uint32_t _reserved[2];
	uint32_t pc;            // +0x10 entry point
	uint32_t gp;            // +0x14 initial $gp
	uint32_t textAddr;      // +0x18 load address
	uint32_t textSize;      // +0x1c payload size in bytes
	uint32_t dataAddr, dataSize;
	uint32_t bssAddr,  bssSize;
	uint32_t spBase,   spOffset;   // +0x30, +0x34
} PSEXEHeader;

#define PSEXE_PAYLOAD_OFFSET 0x800

#ifdef __cplusplus
}
#endif
