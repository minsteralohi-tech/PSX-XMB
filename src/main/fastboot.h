/*
 * PSX-iTests - Fast Boot loader launcher
 *
 * Chain-loads the embedded cdloader.exe (a PS-EXE) directly from within our
 * own executable: copies it to its load address, flushes the cache and starts
 * it with BIOS A0(43h) Exec. Fully self-contained - no separate file on the
 * disc and no BIOS filesystem/LoadExec dependency.
 *
 * The loader currently links at 0x801EA324 (near the top of RAM), but it still
 * uses the shared scratch handoff. That keeps Fast Boot correct if the loader
 * address changes and makes its launch contract identical to UniROM's.
 *
 * launchLoader() never returns.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Chain-load the embedded Fast Boot loader. Does not return.
__attribute__((noreturn)) void launchLoader(void);

#ifdef __cplusplus
}
#endif
