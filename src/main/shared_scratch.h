/*
 * Shared scratch RAM for mutually-exclusive 3D renderers.
 *
 * The GPU cube, standalone model viewer and PS-logo sorter never execute at
 * the same time.  Keeping separate static work buffers for all three wastes
 * scarce PS1 main RAM, so they borrow this single arena instead.
 */
#pragma once

#include <stdint.h>

#define SHARED_3D_SCRATCH_BYTES 12836
#define SHARED_3D_SCRATCH_WORDS \
	((SHARED_3D_SCRATCH_BYTES + sizeof(uint32_t) - 1) / sizeof(uint32_t))

extern uint32_t shared3DScratch[SHARED_3D_SCRATCH_WORDS];
