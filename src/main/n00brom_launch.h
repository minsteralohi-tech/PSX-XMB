/*
 * PSX-iTests - n00bROM integration
 *
 * n00bROM (by Lameguy64) is a homebrew debug/cheat cartridge ROM. Its ROM
 * image (assets/n00brom.rom) is embedded and shipped with this build so it
 * can be flashed to a compatible cheat cartridge.
 *
 * It is a CARTRIDGE ROM, not a PS-EXE: the image begins with a cart
 * signature and is designed to be mapped into the EXP1 ROM region and entered
 * by the BIOS, then it relocates its own body to RAM (PROG_addr 0x801EFFF0).
 * That means it cannot be safely "hot launched" by copying the raw .rom into
 * RAM and jumping to it from within this program - the first bytes of the
 * file are the cart header, not executable code at the target address. Doing
 * so would hang the console. Launching it in-place requires either the
 * flashed cartridge, or an official n00bROM PS-EXE build (chainloaded the
 * same way this project chainloads cdloader.exe).
 *
 * runN00bROMInfo() therefore presents an information screen rather than a
 * blind, unverifiable jump.
 */

#pragma once

#include "main/renderer.h"
#include "main/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

void runN00bROMInfo(RenderContext *ctx, UIState *state, const MenuItem *item);

#ifdef __cplusplus
}
#endif
