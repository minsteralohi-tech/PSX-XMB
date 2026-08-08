/*
 * Shared scratch RAM for mutually-exclusive 3D renderers.
 *
 * The GPU cube, standalone model viewer and PS-logo sorter never execute at
 * the same time.  Keeping separate static work buffers for all three wastes
 * scarce PS1 main RAM, so they borrow this single arena instead.
 */
#pragma once

#include <stdint.h>

/*
 * Sized by the largest consumer, which is xmb_bg.c's PS-logo sorter:
 * PS_LOGO_MAX_FACES (560, the BIOS/Meshy logos) draw-face records, plus the
 * counting sort's buckets and output order.
 *
 * Raised from 12836 when the pose tool's console models gained textures: each
 * draw-face record now also carries the index of the face it came from, so a
 * textured model can look its UVs up at emit time. That is two more bytes per
 * record, four after alignment, and 560 records of it.
 *
 * The _Static_asserts in xmb_bg.c and model_test.c are what actually enforce
 * this; they will fail the build rather than let either renderer overrun.
 */
#define SHARED_3D_SCRATCH_BYTES 15104
#define SHARED_3D_SCRATCH_WORDS \
	((SHARED_3D_SCRATCH_BYTES + sizeof(uint32_t) - 1) / sizeof(uint32_t))

extern uint32_t shared3DScratch[SHARED_3D_SCRATCH_WORDS];
