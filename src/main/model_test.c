/*
 * PSX-iTests - "GPU: Test PlayStation Model" (see model_test.h)
 *
 * Ported from the standalone 3D-model-test project. The transform pipeline,
 * camera constants and spin timing are copied verbatim so the model starts
 * from and moves through exactly the same orientations as the original
 * standalone .exe.
 *
 * The one thing that had to change is VRAM placement. The standalone build
 * put its texture at (SCREEN_WIDTH * 2, 0) = (640, 0), which in this app is
 * occupied by the menu background texture, with the font block right below
 * it - uploading there would corrupt the menu. Both of this screen's
 * textures are therefore relocated above the icon sheets, and the menu's own
 * textures are restored on exit.
 */

#include <stdint.h>
#include <stdbool.h>
#include "common/gpu.h"
#include "common/sio0.h"
#include "main/defs.h"
#include "main/model_test.h"
#include "main/renderer.h"
#include "main/shared_scratch.h"
#include "ps1/gpucmd.h"
#include "ps1/registers.h"
#include "ps1/gte.h"
#include "ps1/cop0.h"
#include "main/trig.h"

#define ONE (1 << 12)   // GTE fixed-point unit

#include "main/model/model_data.h"
#include "main/model/modeltex.h"
#include "main/model/fonttex.h"

/* --- camera / spin constants (identical to the standalone build) --------- */

#define MODEL_CAM_DIST   450
#define MODEL_CAM_PAN_X    0
#define MODEL_CAM_PAN_Y   50
#define START_YAW       1824
#define START_PITCH      670
#define SPIN_SPEED         8   // yaw units advanced per frame while auto-spinning
#define STEP_SPEED        16   // yaw/pitch units per frame under D-pad control
#define PAN_SPEED          4   // vertical movement per frame (Triangle/Cross)
#define ZOOM_SPEED         6   // camera distance change per frame (Square/Circle)
#define ZOOM_MIN         200   // closest the camera may get
#define ZOOM_MAX        1200   // furthest the camera may get
#define PAN_MIN         -250
#define PAN_MAX          350

#define MODEL_OT_SIZE    240   // matches gpu_cube.c's proven-safe CUBE_ORDERING_TABLE_SIZE
#define MODEL_CHAIN_SIZE 1024   // matches gpu_cube.c's proven-safe CUBE_CHAIN_BUFFER_SIZE

/*
 * VRAM layout for this screen (x in halfword units, both textures 16bpp):
 *
 *   menu bg/font/CLUTs : x 640..672   (must not be touched)
 *   category icons     : x 704..768
 *   item icons         : x 768..832
 *   model texture      : x 832..960, y   0..128   <- here
 *   model HUD font     : x 832..864, y 128..158   <- here
 *
 * 832 is a multiple of 64 so U comes out as 0, and both live inside the one
 * 256-line texture page starting at y = 0.
 */
#define MODEL_TEX_X   832
#define MODEL_TEX_Y     0
#define MODEL_FONT_X  832
#define MODEL_FONT_Y  128

typedef struct {
	uint16_t page, clut;
	uint8_t  u, v;
	uint16_t width, height;
} ModelTexture;

typedef struct {
	uint32_t orderingTable[MODEL_OT_SIZE];
	uint32_t data[MODEL_CHAIN_SIZE];
	uint32_t *nextPacket;
} ModelChain;

_Static_assert(
	(sizeof(ModelChain) * 2) <= SHARED_3D_SCRATCH_BYTES,
	"shared 3D scratch arena is too small for Model Tester"
);

/* --- helpers ------------------------------------------------------------- */

// Ordering-table clear via the OTC DMA channel, same approach as the
// spinning-cube test.
static void modelEnableOTDMA(void) {
	DMA_DPCR         |= DMA_DPCR_CH_ENABLE(DMA_OTC);
	DMA_CHCR(DMA_OTC) = 0;
}

static void modelClearOT(uint32_t *table, int numEntries) {
	DMA_MADR(DMA_OTC) = (uint32_t) &table[numEntries - 1];
	DMA_BCR (DMA_OTC) = numEntries;
	DMA_CHCR(DMA_OTC) = 0
		| DMA_CHCR_READ
		| DMA_CHCR_REVERSE
		| DMA_CHCR_MODE_BURST
		| DMA_CHCR_ENABLE
		| DMA_CHCR_TRIGGER;

	while (DMA_CHCR(DMA_OTC) & DMA_CHCR_ENABLE)
		__asm__ volatile("");
}

static uint32_t *allocModelPacket(ModelChain *chain, int zIndex, int numCommands) {
	// Bounds-checked: overrunning data[] would corrupt the ordering table
	// that follows it and hang the GPU on a bad linked-list pointer.
	static uint32_t scratch[16];
	if ((chain->nextPacket + numCommands + 1) >= &(chain->data)[MODEL_CHAIN_SIZE])
		return &scratch[1];

	uint32_t *ptr      = chain->nextPacket;
	chain->nextPacket += numCommands + 1;

	*ptr = gp0_tag(numCommands, (void *) chain->orderingTable[zIndex]);
	chain->orderingTable[zIndex] = gp0_tag(0, ptr);
	return &ptr[1];
}

static void modelUploadTexture(
	ModelTexture *info, const void *data, int x, int y, int width, int height
) {
	sendVRAMData(data, x, y, width, height);
	waitForGPUDMADone();
	GPU_GP0 = gp0_flushCache();

	info->page   = gp0_page(x / 64, y / 256, GP0_BLEND_SEMITRANS, GP0_COLOR_16BPP);
	info->clut   = 0;
	info->u      = (uint8_t) (x % 64);
	info->v      = (uint8_t) (y % 256);
	info->width  = (uint16_t) width;
	info->height = (uint16_t) height;
}

static void modelSetupGTE(int width, int height) {
	cop0_setReg(COP0_STATUS, cop0_getReg(COP0_STATUS) | COP0_STATUS_CU2);
	gte_setControlReg(GTE_OFX,  ((int32_t) width  << 16) / 2);
	gte_setControlReg(GTE_OFY,  ((int32_t) height << 16) / 2);
	gte_setControlReg(GTE_H,    height / 2);
	gte_setControlReg(GTE_ZSF3, MODEL_OT_SIZE / 3);
	gte_setControlReg(GTE_ZSF4, MODEL_OT_SIZE / 4);
}

static void modelMulMatrixByVectors(GTEMatrix *output) {
	gte_command(GTE_CMD_MVMVA | GTE_SF | GTE_MX_RT | GTE_V_V0 | GTE_CV_NONE);
	output->values[0][0] = (int16_t) gte_getDataReg(GTE_IR1);
	output->values[1][0] = (int16_t) gte_getDataReg(GTE_IR2);
	output->values[2][0] = (int16_t) gte_getDataReg(GTE_IR3);
	gte_command(GTE_CMD_MVMVA | GTE_SF | GTE_MX_RT | GTE_V_V1 | GTE_CV_NONE);
	output->values[0][1] = (int16_t) gte_getDataReg(GTE_IR1);
	output->values[1][1] = (int16_t) gte_getDataReg(GTE_IR2);
	output->values[2][1] = (int16_t) gte_getDataReg(GTE_IR3);
	gte_command(GTE_CMD_MVMVA | GTE_SF | GTE_MX_RT | GTE_V_V2 | GTE_CV_NONE);
	output->values[0][2] = (int16_t) gte_getDataReg(GTE_IR1);
	output->values[1][2] = (int16_t) gte_getDataReg(GTE_IR2);
	output->values[2][2] = (int16_t) gte_getDataReg(GTE_IR3);
}

// Rotation applied in the same order as the standalone build.
static void modelRotate(int yaw, int pitch, int roll) {
	static GTEMatrix multiplied;
	int s, c;

	if (yaw) {
		s = isin(yaw); c = icos(yaw);
		gte_setColumnVectors(c, -s, 0,  s, c, 0,  0, 0, ONE);
		modelMulMatrixByVectors(&multiplied);
		gte_loadRotationMatrix(&multiplied);
	}
	if (pitch) {
		s = isin(pitch); c = icos(pitch);
		gte_setColumnVectors(c, 0, s,  0, ONE, 0,  -s, 0, c);
		modelMulMatrixByVectors(&multiplied);
		gte_loadRotationMatrix(&multiplied);
	}
	if (roll) {
		s = isin(roll); c = icos(roll);
		gte_setColumnVectors(ONE, 0, 0,  0, c, -s,  0, s, c);
		modelMulMatrixByVectors(&multiplied);
		gte_loadRotationMatrix(&multiplied);
	}
}

/* --- entry point --------------------------------------------------------- */

void runModelTest(RenderContext *ctx, UIState *state, const MenuItem *item) {
	(void) state;
	(void) item;

	int width  = ctx->screenWidth;
	int height = ctx->screenHeight;
	ModelChain *modelChains = (ModelChain *) shared3DScratch;

	ModelTexture texture, font;
	modelUploadTexture(&texture, modeltexTextureData,
		MODEL_TEX_X, MODEL_TEX_Y, MODELTEX_WIDTH, MODELTEX_HEIGHT);
	modelUploadTexture(&font, fonttexTextureData,
		MODEL_FONT_X, MODEL_FONT_Y, FONTTEX_WIDTH, FONTTEX_HEIGHT);

	modelSetupGTE(width, height);
	modelEnableOTDMA();

	int  camYaw   = START_YAW;
	int  camPitch = START_PITCH;
	int  camDist  = MODEL_CAM_DIST;   // Square / Circle zoom out / in
	int  camPanY  = MODEL_CAM_PAN_Y;  // Triangle / Cross move up / down
	bool manualMode = false;
	bool prevSelect = false, prevStart = false;
	int  autoFrameCounter = 0;
	bool usingSecondFrame = false;

	// Wait for the button that opened this screen to be released.
	while (pollController(0) | pollController(1))
		;

	for (;;) {
		uint16_t buttons = pollController(0) | pollController(1);

		// L1 or R1 exits. The face buttons now move/zoom the model.
		if (buttons & (PAD_BTN_L1 | PAD_BTN_R1))
			break;

		// Triangle / Cross raise and lower the model; Square / Circle zoom
		// out and in. Held, not edge-triggered, so they move smoothly.
		if (buttons & PAD_BTN_TRIANGLE) camPanY -= PAN_SPEED;
		if (buttons & PAD_BTN_CROSS)    camPanY += PAN_SPEED;
		if (buttons & PAD_BTN_SQUARE)   camDist += ZOOM_SPEED;
		if (buttons & PAD_BTN_CIRCLE)   camDist -= ZOOM_SPEED;

		if (camDist < ZOOM_MIN) camDist = ZOOM_MIN;
		if (camDist > ZOOM_MAX) camDist = ZOOM_MAX;
		if (camPanY < PAN_MIN)  camPanY = PAN_MIN;
		if (camPanY > PAN_MAX)  camPanY = PAN_MAX;

		bool selectHeld = (buttons & PAD_BTN_SELECT) != 0;
		bool startHeld  = (buttons & PAD_BTN_START)  != 0;
		bool selectPressed = selectHeld && !prevSelect;
		bool startPressed  = startHeld  && !prevStart;
		prevSelect = selectHeld;
		prevStart  = startHeld;

		// Where auto-spin would be right now; also used to hand manual mode
		// a seamless starting angle so there is no visual jump.
		int autoYaw = (START_YAW + autoFrameCounter * SPIN_SPEED) & 4095;

		if (selectPressed && !manualMode) {
			camYaw     = autoYaw;
			camPitch   = START_PITCH;
			manualMode = true;
		}
		if (startPressed) {
			manualMode       = false;
			autoFrameCounter = 0;
		}

		if (manualMode) {
			if (buttons & PAD_BTN_LEFT)  camYaw   -= STEP_SPEED;
			if (buttons & PAD_BTN_RIGHT) camYaw   += STEP_SPEED;
			if (buttons & PAD_BTN_UP)    camPitch -= STEP_SPEED;
			if (buttons & PAD_BTN_DOWN)  camPitch += STEP_SPEED;
			camYaw   &= 4095;
			camPitch &= 4095;
		} else {
			camYaw   = autoYaw;
			camPitch = START_PITCH;
			autoFrameCounter++;
		}

		int bufferX = usingSecondFrame ? width : 0;
		int bufferY = 0;

		ModelChain *chain = &modelChains[usingSecondFrame ? 1 : 0];
		usingSecondFrame  = !usingSecondFrame;

		// Display this buffer now; the frame just built is the one being
		// shown, exactly as the standalone build sequences it.
		GPU_GP1 = gp1_fbOffset(bufferX, bufferY);

		modelClearOT(chain->orderingTable, MODEL_OT_SIZE);
		chain->nextPacket = chain->data;

		uint32_t *ptr;

		// Background fill, matching the standalone build's dark blue.
		ptr = allocModelPacket(chain, MODEL_OT_SIZE - 1, 3);
		ptr[0] = gp0_rgb(32, 32, 64) | gp0_vramFill();
		ptr[1] = gp0_xy(bufferX, bufferY);
		ptr[2] = gp0_xy(width, height);

		/*
		 * Drawing-area setup. This is REQUIRED and its absence was the bug
		 * behind the striped, flickering output: without it the GPU keeps
		 * whatever clip window and origin the menu renderer last set, so
		 * this screen's triangles were written to the wrong part of VRAM -
		 * straight over the texture that was being sampled, which is why
		 * the model showed as garbage stripes instead of its texture.
		 *
		 * Allocated AFTER the fill at the same ordering-table index, and
		 * packets at one index are drawn newest-first, so this executes
		 * before the fill - the clip window is set up first, as it must be.
		 */
		ptr = allocModelPacket(chain, MODEL_OT_SIZE - 1, 4);
		ptr[0] = gp0_setPage(0, true, false);
		ptr[1] = gp0_fbOffset1(bufferX, bufferY);
		ptr[2] = gp0_fbOffset2(bufferX + width - 1, bufferY + height - 1);
		ptr[3] = gp0_fbOrigin(bufferX, bufferY);

		// Camera: fixed distance and pan, never touched by input.
		gte_setControlReg(GTE_TRX, MODEL_CAM_PAN_X);
		gte_setControlReg(GTE_TRY, camPanY);
		gte_setControlReg(GTE_TRZ, camDist);

		gte_setRotationMatrix(ONE, 0, 0,  0, ONE, 0,  0, 0, ONE);
		modelRotate(0, camYaw, 0);      // orbit yaw
		modelRotate(0, 0, camPitch);    // orbit pitch
		modelRotate(1024, 0, 1024);     // lay the model flat and sideways

		for (int i = 0; i < face_count; i++) {
			const MESH_FACE *face = &model_faces[i];

			gte_loadV0((const GTEVector16 *) &model_vertices[face->v0]);
			gte_loadV1((const GTEVector16 *) &model_vertices[face->v1]);
			gte_loadV2((const GTEVector16 *) &model_vertices[face->v2]);
			gte_command(GTE_CMD_RTPT | GTE_SF);

			uint32_t xy0 = gte_getDataReg(GTE_SXY0);
			uint32_t xy1 = gte_getDataReg(GTE_SXY1);
			uint32_t xy2 = gte_getDataReg(GTE_SXY2);

			gte_command(GTE_CMD_AVSZ3 | GTE_SF);
			int zIndex = (int) gte_getDataReg(GTE_OTZ);
			if (zIndex >= MODEL_OT_SIZE - 1)
				continue;
			if (zIndex < 1)
				zIndex = 1;

			ptr = allocModelPacket(chain, zIndex, 7);
			ptr[0] = gp0_shadedTriangle(false, true, false) | gp0_rgb(128, 128, 128);
			ptr[1] = xy0;
			ptr[2] = ((uint32_t) texture.clut << 16)
			       | ((uint32_t) (texture.v + face->tv0) << 8)
			       | (uint32_t) (texture.u + face->tu0);
			ptr[3] = xy1;
			ptr[4] = ((uint32_t) texture.page << 16)
			       | ((uint32_t) (texture.v + face->tv1) << 8)
			       | (uint32_t) (texture.u + face->tu1);
			ptr[5] = xy2;
			ptr[6] = ((uint32_t) (texture.v + face->tv2) << 8)
			       | (uint32_t) (texture.u + face->tu2);
		}

		waitForGP0Ready();
		waitForVSync();
		sendGPULinkedList(&(chain->orderingTable)[MODEL_OT_SIZE - 1]);
	}

	// Put the menu's own textures back - this screen overwrote part of VRAM.
	reloadTextures(ctx);

	while (pollController(0) | pollController(1))
		;
}
