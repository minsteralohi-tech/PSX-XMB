/*
 * PSX-iTests - UniROM 8.0 launcher
 *
 * Chain-loads the embedded unirom_bin.exe (a real, unmodified UniROM build,
 * a PS-EXE) directly from within our own executable, via the shared
 * hand-off path in handoff.c: quiesce the hardware, copy a tiny
 * position-independent stub to scratch RAM, let that stub copy/clear the
 * image, flush the cache, and enter it through BIOS A0(43h) Exec. Same path
 * Fast Boot's cdloader.exe now uses.
 *
 * This exists as the reliable alternative to this project's own SIO Loader
 * - and, because UniROM links at 0x801D0000, it is also the only way to
 * serial-load a PS-EXE that links at 0x80010000 (i.e. most of them) without
 * that program landing on top of this app.
 *
 * FIXED: the first version of this launcher left UniROM frozen the instant
 * it was selected. It zeroed DMA_DPCR while renderer.c's endFrame() still
 * had a display-list DMA in flight, stranding GPU DMA channel 2 with its
 * busy bit set. UniROM's GPU init then spun forever at 0x801DB40C waiting
 * on GPUSTAT bit 26, which never came back. cdloader.exe only escaped the
 * same fate because its own setupGPU() happens to clear that channel on
 * start-up. handoff.h has the full write-up.
 *
 * launchUniROM() and returnToUniROMCart() never return.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Chain-load the embedded UniROM 8.0 build. Does not return.
__attribute__((noreturn)) void launchUniROM(void);

// For consoles booted through a real, physically-installed UniROM
// cartridge: hand control back to it via a real soft-reset instead of
// copying a redundant second UniROM image into RAM. Use this - not
// launchUniROM() - when a UniROM cart is what's actually installed; see
// unirom_launch.c for why the two do not mix safely. Does not return.
__attribute__((noreturn)) void returnToUniROMCart(void);

#ifdef __cplusplus
}
#endif
