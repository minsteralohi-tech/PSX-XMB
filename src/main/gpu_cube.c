/*
 * PSX-iTests - GPU 3D (GTE) test
 *
 * Ported from ps1-bare-metal's 08_spinningCube example by spicyjpeg, adapted
 * to run as a menu screen (exits on button press instead of running
 * forever) and to be fully self-contained rather than sharing the project's
 * common/gpu.c GPU driver.
 *
 * Why self-contained: this test needs a Z-sorted ordering table (to draw
 * the cube's faces back-to-front) and a matching 3-argument
 * allocateGP0Packet(chain, zIndex, numCommands). The shared common/gpu.c
 * used by the rest of the menu has neither - its GPUDMAChain has no
 * ordering table and its allocateGP0Packet() only takes 2 arguments,
 * because the menu's flat 2D UI never needed depth sorting. Rather than
 * change the shared driver (and risk breaking every other screen that
 * depends on its exact current behavior), this file defines its own
 * private ordering-table-aware types and packet allocator, under different
 * names so they can't collide with or accidentally be used in place of the
 * shared ones. The low-level primitives that ARE compatible (waitForVSync,
 * waitForGP0Ready, sendGPULinkedList) are reused as-is from common/gpu.c.
 *
 * One more thing the shared setupGPU() doesn't do that this test needs: it
 * never enables the DMA_OTC channel (used to rapidly build the ordering
 * table), since nothing else in the project uses it. This test enables it
 * locally on entry instead of touching the shared GPU init code.
 */

#include <stdbool.h>
#include <stdint.h>
#include "common/gpu.h"
#include "common/sio0.h"
#include "main/gpu_cube.h"
#include "main/mainmenu.h"
#include "main/trig.h"
#include "ps1/cop0.h"
#include "ps1/gpucmd.h"
#include "ps1/gte.h"
#include "ps1/registers.h"

#define GTE_UNIT (1 << 12)

#define CUBE_CHAIN_BUFFER_SIZE   1024
#define CUBE_ORDERING_TABLE_SIZE  240

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

typedef struct {
	uint32_t data[CUBE_CHAIN_BUFFER_SIZE];
	uint32_t orderingTable[CUBE_ORDERING_TABLE_SIZE];
	uint32_t *nextPacket;
} CubeDMAChain;

/* Private GPU helpers (ordering-table-aware, not shared with common/gpu.c) */

static void enableOrderingTableDMA(void) {
	DMA_DPCR         |= DMA_DPCR_CH_ENABLE(DMA_OTC);
	DMA_CHCR(DMA_OTC)  = 0;
}

static void clearCubeOrderingTable(uint32_t *table, int numEntries) {
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

// Scratch packet used when the chain is full, so callers always get a
// writable buffer instead of scribbling past the end of chain->data.
static uint32_t cubeOverflowScratch[16];

static uint32_t *allocateCubeGP0Packet(
	CubeDMAChain *chain,
	int          zIndex,
	int          numCommands
) {
	/*
	 * Bounds check. orderingTable[] sits immediately after data[] inside
	 * CubeDMAChain, so overrunning data[] used to silently overwrite the
	 * ordering table - which is the DMA linked list the GPU walks. The
	 * GPU would then follow a corrupted "next packet" pointer and never
	 * reach a terminator, hanging the console hard with no way out except
	 * a power cycle. Dropping the packet keeps the list intact.
	 */
	if ((chain->nextPacket + numCommands + 1)
		>= &(chain->data)[CUBE_CHAIN_BUFFER_SIZE])
		return &cubeOverflowScratch[1];

	uint32_t *ptr      = chain->nextPacket;
	chain->nextPacket += numCommands + 1;

	*ptr = gp0_tag(numCommands, (void *) chain->orderingTable[zIndex]);
	chain->orderingTable[zIndex] = gp0_tag(0, ptr);

	return &ptr[1];
}

/* GTE setup, ported as-is from the reference example */

static void setupGTE(int width, int height) {
	cop0_setReg(COP0_STATUS, cop0_getReg(COP0_STATUS) | COP0_STATUS_CU2);

	gte_setControlReg(GTE_OFX, (width  << 16) / 2);
	gte_setControlReg(GTE_OFY, (height << 16) / 2);

	int focalLength = (width < height) ? width : height;
	gte_setControlReg(GTE_H, focalLength / 2);

	gte_setControlReg(GTE_ZSF3, CUBE_ORDERING_TABLE_SIZE / 3);
	gte_setControlReg(GTE_ZSF4, CUBE_ORDERING_TABLE_SIZE / 4);
}

static void multiplyCurrentMatrixByVectors(GTEMatrix *output) {
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

static void rotateCurrentMatrix(int yaw, int pitch, int roll) {
	static GTEMatrix multiplied;
	int s, c;

	if (yaw) {
		s = isin(yaw);
		c = icos(yaw);

		gte_setColumnVectors(
			c, -s,        0,
			s,  c,        0,
			0,  0, GTE_UNIT
		);
		multiplyCurrentMatrixByVectors(&multiplied);
		gte_loadRotationMatrix(&multiplied);
	}
	if (pitch) {
		s = isin(pitch);
		c = icos(pitch);

		gte_setColumnVectors(
			 c,        0, s,
			 0, GTE_UNIT, 0,
			-s,        0, c
		);
		multiplyCurrentMatrixByVectors(&multiplied);
		gte_loadRotationMatrix(&multiplied);
	}
	if (roll) {
		s = isin(roll);
		c = icos(roll);

		gte_setColumnVectors(
			GTE_UNIT, 0,  0,
			       0, c, -s,
			       0, s,  c
		);
		multiplyCurrentMatrixByVectors(&multiplied);
		gte_loadRotationMatrix(&multiplied);
	}
}

/* Cube model data, ported as-is from the reference example */

typedef struct {
	uint8_t  vertices[4];
	uint32_t color;
} Face;

#define NUM_CUBE_VERTICES 8
#define NUM_CUBE_FACES    6

static const GTEVector16 cubeVertices[NUM_CUBE_VERTICES] = {
	{ .x = -32, .y = -32, .z = -32 },
	{ .x =  32, .y = -32, .z = -32 },
	{ .x = -32, .y =  32, .z = -32 },
	{ .x =  32, .y =  32, .z = -32 },
	{ .x = -32, .y = -32, .z =  32 },
	{ .x =  32, .y = -32, .z =  32 },
	{ .x = -32, .y =  32, .z =  32 },
	{ .x =  32, .y =  32, .z =  32 }
};

static const Face cubeFaces[NUM_CUBE_FACES] = {
	{ .vertices = { 0, 1, 2, 3 }, .color = 0x0000ff },
	{ .vertices = { 6, 7, 4, 5 }, .color = 0x00ff00 },
	{ .vertices = { 4, 5, 0, 1 }, .color = 0x00ffff },
	{ .vertices = { 7, 6, 3, 2 }, .color = 0xff0000 },
	{ .vertices = { 6, 4, 2, 0 }, .color = 0xff00ff },
	{ .vertices = { 5, 7, 1, 3 }, .color = 0xffff00 }
};

/* Menu callback */

void runGPUCubeTest(
	RenderContext  *ctx,
	UIState        *state,
	const MenuItem *item
) {
	(void) ctx;
	(void) state;
	(void) item;

	enableOrderingTableDMA();
	setupGTE(SCREEN_WIDTH, SCREEN_HEIGHT);

	static CubeDMAChain dmaChains[2];
	bool                usingSecondFrame = false;
	int                 frameCounter     = 0;

	// Debounce: wait for the button that opened this screen to be released.
	while (pollController(0) | pollController(1))
		;

	for (;;) {
		int bufferX = usingSecondFrame ? SCREEN_WIDTH : 0;
		int bufferY = 0;

		CubeDMAChain *chain = &dmaChains[usingSecondFrame];
		usingSecondFrame     = !usingSecondFrame;

		uint32_t *ptr;

		GPU_GP1 = gp1_fbOffset(bufferX, bufferY);

		clearCubeOrderingTable(chain->orderingTable, CUBE_ORDERING_TABLE_SIZE);
		chain->nextPacket = chain->data;

		gte_setControlReg(GTE_TRX,   0);
		gte_setControlReg(GTE_TRY,   0);
		gte_setControlReg(GTE_TRZ, 128);
		gte_setRotationMatrix(
			GTE_UNIT,        0,        0,
			       0, GTE_UNIT,        0,
			       0,        0, GTE_UNIT
		);

		rotateCurrentMatrix(0, frameCounter * 16, frameCounter * 12);
		frameCounter++;

		for (int i = 0; i < NUM_CUBE_FACES; i++) {
			const Face *face = &cubeFaces[i];

			gte_loadV0(&cubeVertices[face->vertices[0]]);
			gte_loadV1(&cubeVertices[face->vertices[1]]);
			gte_loadV2(&cubeVertices[face->vertices[2]]);
			gte_command(GTE_CMD_RTPT | GTE_SF);

			gte_command(GTE_CMD_NCLIP);

			if (((int) gte_getDataReg(GTE_MAC0)) <= 0)
				continue;

			uint32_t xy0 = gte_getDataReg(GTE_SXY0);

			gte_loadV0(&cubeVertices[face->vertices[3]]);
			gte_command(GTE_CMD_RTPS | GTE_SF);

			gte_command(GTE_CMD_AVSZ4 | GTE_SF);
			int zIndex = (int) gte_getDataReg(GTE_OTZ);

			if ((zIndex < 0) || (zIndex >= CUBE_ORDERING_TABLE_SIZE))
				continue;

			ptr    = allocateCubeGP0Packet(chain, zIndex, 5);
			ptr[0] = face->color | gp0_shadedQuad(false, false, false);
			ptr[1] = xy0;
			gte_storeDataReg(GTE_SXY0, 2 * 4, ptr);
			gte_storeDataReg(GTE_SXY1, 3 * 4, ptr);
			gte_storeDataReg(GTE_SXY2, 4 * 4, ptr);
		}

		ptr    = allocateCubeGP0Packet(chain, CUBE_ORDERING_TABLE_SIZE - 1, 3);
		ptr[0] = gp0_rgb(64, 64, 64) | gp0_vramFill();
		ptr[1] = gp0_xy(bufferX, bufferY);
		ptr[2] = gp0_xy(SCREEN_WIDTH, SCREEN_HEIGHT);

		ptr    = allocateCubeGP0Packet(chain, CUBE_ORDERING_TABLE_SIZE - 1, 4);
		ptr[0] = gp0_setPage(0, true, false);
		ptr[1] = gp0_fbOffset1(bufferX, bufferY);
		ptr[2] = gp0_fbOffset2(
			bufferX + SCREEN_WIDTH  - 1,
			bufferY + SCREEN_HEIGHT - 1
		);
		ptr[3] = gp0_fbOrigin(bufferX, bufferY);

		waitForGP0Ready();
		waitForVSync();
		sendGPULinkedList(
			&(chain->orderingTable)[CUBE_ORDERING_TABLE_SIZE - 1]
		);

		if (pollController(0) | pollController(1))
			break;
	}

	// Flush button state before handing control back to the outer menu
	// system - see the matching comment in memcard.c's
	// runMemoryCardManager() for the full explanation. Without this,
	// whichever button you exited with is still held when control
	// returns, and the outer menu's stale lastButtons misreads it as a
	// fresh "confirm" press on this same still-highlighted item,
	// immediately re-opening this screen.
	while (pollController(0) | pollController(1))
		;

	// Deliberately no enterMainMenu() call here - state->currentMenu and
	// state->menuCursor were never touched anywhere in this function
	// (this screen renders through its own self-contained loop
	// instead), so they still correctly point at whichever item was
	// selected to get here.
}
