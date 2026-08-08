#ifndef PS_CONSOLE_MODEL_H
#define PS_CONSOLE_MODEL_H

/*
 * Textured console models for the PS Logo Pose tool.
 *
 * These share PSLogoVertex with the flat-shaded logo models, because the
 * transform stage is identical - the GTE loads three int16 triples per triangle
 * and neither knows nor cares what happens afterwards. Only the face record
 * differs: where PSLogoFace carries one flat colour for the whole triangle,
 * this carries a texel coordinate per corner, which is the entire difference
 * between the console reading as a PlayStation and reading as a white brick.
 *
 * WHY THIS IS A SEPARATE TYPE RATHER THAN THREE MORE FIELDS ON PSLogoFace
 *
 * The flat models are the ones the boot intro actually ships (model A is
 * hardware-tuned and finalized), and they are also the big ones - 560 faces
 * each against these two's 296 and 80. Widening PSLogoFace by six bytes would
 * charge that to every one of them, in a 2 MB console where this project has
 * already had to drop a BGM track and a test suite to fit. Two types cost one
 * branch in drawPSLogoFaces() and nothing at all in RAM.
 *
 * u/v are texel coordinates within the model's own texture page, already
 * V-flipped by tools/glb2console.py - see that file for how the convention was
 * established against the known-good model_test.c pair. They are added to the
 * TextureInfo's own u/v base at draw time, so a texture may sit anywhere in
 * VRAM without regenerating these tables.
 */

#include <stdint.h>

#include "main/model/ps_logo_model.h"

typedef struct {
	uint16_t v0, v1, v2;
	uint8_t  u0, t0, u1, t1, u2, t2;
} PSConsoleFace;

#endif // PS_CONSOLE_MODEL_H
