/*
 * Host tests for src/main/app_launch.c's planner.
 *
 * Builds the dashboard's own planner source on a PC and checks the decisions
 * that decide whether a launch works or produces a black screen: arena
 * selection against every target this project actually ships, overlap rules,
 * zero-fill range construction, and rejection of malformed headers.
 *
 *   make -C tools -f Makefile.tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "main/app_launch.h"

/*
 * Host stand-ins for the three console routines the handoff calls. Their only
 * job is to exist: with them present, the whole of app_launch.c - including
 * runDirectLaunch() and runStagedLaunch() - is compiled here, so a missing
 * prototype or a wrong argument type is a test failure rather than something
 * CI discovers ten minutes into a cross build. (That is exactly how
 * jumpToLoadedEXE() got through: it was defined in handoff.c but never
 * declared in handoff.h, and the launch paths were excluded from this build.)
 *
 * Nothing calls them - the tests only exercise planEmbeddedApp().
 */
void quiesceForHandoff(void) {}
void flushCache(void) {}

__attribute__((noreturn)) void jumpToLoadedEXE(uint32_t pc, uint32_t gp,
                                               uint32_t sp) {
	(void) pc;
	(void) gp;
	(void) sp;
	printf("  FAIL jumpToLoadedEXE() reached in a host test\n");
	exit(1);
}

/* Stand-in for the real stage 1 blob from app_stub.s. */
const uint32_t appStubStart[16];
const uint32_t appStubEnd[1];

extern uint32_t appTestImageEnd;
extern uint32_t appTestTextEnd;
extern uint32_t appTestStack;

static int failures;
static int checks;

#define CHECK(cond) do {                                               \
	checks++;                                                          \
	if (!(cond)) {                                                     \
		failures++;                                                    \
		printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
	}                                                                  \
} while (0)

#define CHECK_RESULT(got, want) do {                                   \
	AppPlanResult g_ = (got);                                          \
	checks++;                                                          \
	if (g_ != (want)) {                                                \
		failures++;                                                    \
		printf("  FAIL %s:%d: expected %s, got %s\n", __FILE__,        \
		       __LINE__, appPlanResultText(want),                      \
		       appPlanResultText(g_));                                 \
	}                                                                  \
} while (0)

/* A fake embedded blob: 2048-byte header plus a little payload. Its address
 * inside this test process stands in for .rodata inside the dashboard. */
static uint8_t exeBuffer[PSEXE_PAYLOAD_OFFSET + 0x2000];

static void makeExe(uint32_t pc, uint32_t load, uint32_t size,
                    uint32_t bssAddr, uint32_t bssSize, uint32_t sp) {
	PSEXEHeader *h = (PSEXEHeader *) exeBuffer;

	memset(exeBuffer, 0, sizeof(exeBuffer));
	memcpy(h->magic, "PS-X EXE", 8);
	h->pc       = pc;
	h->textAddr = load;
	h->textSize = size;
	h->bssAddr  = bssAddr;
	h->bssSize  = bssSize;
	h->spBase   = sp;
}

static int fillsCover(const AppLaunchPlan *plan, uint32_t addr) {
	for (uint32_t i = 0; i < plan->fillCount; i++) {
		if (addr >= plan->fillStart[i] &&
		    addr < plan->fillStart[i] + plan->fillBytes[i])
			return 1;
	}

	return 0;
}

static void testStandaloneSIOLoader(void) {
	printf("standalone SIO loader (0x801b0000, 6144 bytes)\n");

	/* Exactly the header of the shipped assets/sioloader.exe. */
	makeExe(0x801b0000, 0x801b0000, 0x1800, 0x801b1770, 2064, 0x801bdff0);

	AppLaunchPlan plan;
	CHECK_RESULT(planEmbeddedApp(exeBuffer, 0, &plan), APP_PLAN_OK);

	CHECK(plan.dest == 0x801b0000);
	CHECK(plan.destEnd == 0x801b1800);
	CHECK(plan.entry == 0x801b0000);
	CHECK(plan.sp == 0x801bdff0);

	/* The loader lands above the dashboard's .text and stack, so no
	 * trampoline is needed at all - this takes the same direct copy-and-jump
	 * that Fast Boot and UniROM already use successfully on hardware. */
	CHECK(plan.useStage1 == 0);
	CHECK(plan.arena == 0);

	/* Without eraseRam the only fill is the PS-EXE memfill, rounded up to a
	 * whole number of words. */
	CHECK(plan.fillCount == 1);
	CHECK(plan.fillStart[0] == 0x801b1770);
	CHECK(plan.fillBytes[0] == ((2064 + 3) & ~3u));
}

static void testUniROM(void) {
	printf("UniROM 8.0.K (0x801d0000, 135168 bytes)\n");

	makeExe(0x801d0000, 0x801d0000, 0x21000, 0, 0, 0x801fff00);

	AppLaunchPlan plan;
	CHECK_RESULT(planEmbeddedApp(exeBuffer, 0, &plan), APP_PLAN_OK);

	/* Same as the SIO loader: comfortably above the live region, so the
	 * direct path applies - which is exactly what the shipping UniROM menu
	 * entry already does. */
	CHECK(plan.useStage1 == 0);
	CHECK(plan.destEnd == 0x801f1000);
	CHECK(plan.fillCount == 0);
}

static void testCDLoader(void) {
	printf("cdloader.exe (0x801ea300, occupies the top of RAM)\n");

	makeExe(0x801ea300, 0x801ea300, 0x15c00, 0, 0, 0);

	AppLaunchPlan plan;
	CHECK_RESULT(planEmbeddedApp(exeBuffer, 0, &plan), APP_PLAN_OK);

	CHECK(plan.useStage1 == 0);

	/* spBase == 0 in the header means the BIOS default. */
	CHECK(plan.sp == 0x801fff00);
}

static void testOrdinaryHomebrew(void) {
	printf("ordinary homebrew at 0x80010000 (lands on the dashboard)\n");

	makeExe(0x80010000, 0x80010000, 0x40000, 0x80050000, 0x1000, 0);

	AppLaunchPlan plan;
	CHECK_RESULT(planEmbeddedApp(exeBuffer, 0, &plan), APP_PLAN_OK);

	/* This one really does overwrite the copier and its stack, so it is the
	 * case stage 1 exists for. */
	CHECK(plan.useStage1 == 1);
	CHECK(plan.arena == 0x801ff000);
	CHECK(plan.arena >= plan.liveEnd);
}

static void testArenaAvoidsSourceBlob(void) {
	printf("arena never lands on the embedded blob it is copying from\n");

	/* A target loading exactly where the source blob happens to sit is the
	 * pathological case; the arena must still clear both. */
	makeExe(0x80010000, 0x80010000, 0x1000, 0, 0, 0);

	AppLaunchPlan plan;
	CHECK_RESULT(planEmbeddedApp(exeBuffer, 0, &plan), APP_PLAN_OK);

	uint32_t arenaStart = plan.arena;
	uint32_t arenaEnd   = plan.arena + APP_ARENA_SIZE;

	CHECK(!(arenaStart < plan.destEnd && plan.dest < arenaEnd));
	CHECK(!(arenaStart < plan.src + plan.bodySize && plan.src < arenaEnd));
}

static void testArenaClearsDashboardImage(void) {
	printf("arena is never inside the dashboard's own image\n");

	makeExe(0x80010000, 0x80010000, 0x1000, 0, 0, 0);

	AppLaunchPlan plan;

	/*
	 * A huge .bss must NOT push the arena away: only .text and the live
	 * stack matter once quiesce has run. Getting this wrong is what forced
	 * the fallback into BIOS kernel scratch - the one arrangement that has
	 * never worked on this console.
	 */
	appTestImageEnd = 0x801f8000;
	CHECK_RESULT(planEmbeddedApp(exeBuffer, 0, &plan), APP_PLAN_OK);
	CHECK(plan.arena == 0x801ff000);

	/* But the stack really does move it. */
	appTestStack = 0x801fe000;
	CHECK_RESULT(planEmbeddedApp(exeBuffer, 0, &plan), APP_PLAN_OK);
	CHECK(plan.arena == 0x8000c000);

	appTestStack = 0x8019e0c8;
	appTestImageEnd = 0x801a6000;
}

static void testEraseRamFills(void) {
	printf("erase-RAM fill list\n");

	makeExe(0x801b0000, 0x801b0000, 0x1800, 0x801b1770, 2064, 0x801bdff0);

	AppLaunchPlan plan;
	CHECK_RESULT(planEmbeddedApp(exeBuffer, 1, &plan), APP_PLAN_OK);

	/* Erasing RAM always needs stage 1: the code doing the erasing would
	 * otherwise be its own first casualty. */
	CHECK(plan.useStage1 == 1);

	/* Everything below the loader, everything above it, split around the
	 * arena, plus the BSS memfill. */
	CHECK(plan.fillCount >= 3);

	/* The dashboard itself is erased... */
	CHECK(fillsCover(&plan, APP_RAM_BASE));
	CHECK(fillsCover(&plan, 0x80100000));

	/* ...but never the code we just copied. Note the tail of the padded
	 * payload legitimately IS covered: this loader's own .bss begins at
	 * 0x801b1770, inside the 0x1800-byte padded body, so the memfill zeroes
	 * it exactly as the BIOS would after a LoadExec. Only the region below
	 * the BSS start must survive. */
	CHECK(!fillsCover(&plan, plan.dest));
	CHECK(!fillsCover(&plan, plan.dest + 0x1000));
	CHECK(!fillsCover(&plan, 0x801b176c));

	/* nor stage 1, its parameters or its fill list, */
	CHECK(!fillsCover(&plan, plan.arena));
	CHECK(!fillsCover(&plan, plan.arena + APP_ARENA_PARAM_OFF));
	CHECK(!fillsCover(&plan, plan.arena + APP_ARENA_SIZE - 4));

	/* nor anything the BIOS owns below 0x80010000. */
	CHECK(!fillsCover(&plan, 0x8000c800));
	CHECK(!fillsCover(&plan, 0x80000080));
	CHECK(!fillsCover(&plan, 0x80000200));

	/* The gap on either side of the arena is still cleared. */
	CHECK(fillsCover(&plan, plan.arena - 4));

	for (uint32_t i = 0; i < plan.fillCount; i++) {
		CHECK(plan.fillStart[i] >= APP_RAM_BASE);
		CHECK(plan.fillStart[i] + plan.fillBytes[i] <= APP_RAM_TOP);
	}
}

static void testRejections(void) {
	printf("rejections\n");

	AppLaunchPlan plan;

	makeExe(0x801b0000, 0x801b0000, 0x1800, 0, 0, 0);
	exeBuffer[3] = 'Y';
	CHECK_RESULT(planEmbeddedApp(exeBuffer, 0, &plan), APP_PLAN_BAD_MAGIC);

	makeExe(0x801b0000, 0x801b0000, 0, 0, 0, 0);
	CHECK_RESULT(planEmbeddedApp(exeBuffer, 0, &plan), APP_PLAN_BAD_SIZE);

	/* Into BIOS/kernel RAM. */
	makeExe(0x80000500, 0x80000500, 0x1000, 0, 0, 0);
	CHECK_RESULT(planEmbeddedApp(exeBuffer, 0, &plan), APP_PLAN_DEST_RANGE);

	/* Off the end of 2 MB. */
	makeExe(0x801f0000, 0x801f0000, 0x20000, 0, 0, 0);
	CHECK_RESULT(planEmbeddedApp(exeBuffer, 0, &plan), APP_PLAN_DEST_RANGE);

	/* Wrapping past 2^32 rather than looking like a tiny range near zero. */
	makeExe(0x801ffffc, 0x801ffffc, 0xfffff000, 0, 0, 0);
	CHECK_RESULT(planEmbeddedApp(exeBuffer, 0, &plan), APP_PLAN_DEST_RANGE);

	makeExe(0x801b0002, 0x801b0002, 0x1000, 0, 0, 0);
	CHECK_RESULT(planEmbeddedApp(exeBuffer, 0, &plan), APP_PLAN_UNALIGNED);

	/* Entry outside the loaded image. */
	makeExe(0x80040000, 0x801b0000, 0x1000, 0, 0, 0);
	CHECK_RESULT(planEmbeddedApp(exeBuffer, 0, &plan), APP_PLAN_ENTRY_RANGE);

	makeExe(0x801b0000, 0x801b0000, 0x1000, 0, 0, 0x00000010);
	CHECK_RESULT(planEmbeddedApp(exeBuffer, 0, &plan), APP_PLAN_SP_RANGE);

	makeExe(0x801b0000, 0x801b0000, 0x1000, 0x801ff000, 0x8000, 0);
	CHECK_RESULT(planEmbeddedApp(exeBuffer, 0, &plan), APP_PLAN_BSS_RANGE);
}

static void testBssPushesArenaAside(void) {
	printf("a target whose BSS covers the top of RAM moves the arena\n");

	/* Payload low, but a memfill that runs right through 0x801ff000 and
	 * past it - 0xf000 would end exactly at the arena and legitimately not
	 * overlap, so the range has to genuinely cover it. */
	makeExe(0x80010000, 0x80010000, 0x1000, 0x801f0000, 0xf800, 0);

	AppLaunchPlan plan;
	CHECK_RESULT(planEmbeddedApp(exeBuffer, 0, &plan), APP_PLAN_OK);

	if (plan.useStage1) {
		CHECK(plan.arena != 0x801ff000);
		CHECK(plan.arena + APP_ARENA_SIZE <= 0x801f0000 ||
		      plan.arena < APP_RAM_BASE);
	}
}

static void testPhysicalAddressesAreCanonicalised(void) {
	printf("KUSEG and KSEG1 header addresses become KSEG0\n");

	makeExe(0x001b0000, 0x001b0000, 0x1000, 0, 0, 0);

	AppLaunchPlan plan;
	CHECK_RESULT(planEmbeddedApp(exeBuffer, 0, &plan), APP_PLAN_OK);
	CHECK(plan.dest == 0x801b0000);
	CHECK(plan.entry == 0x801b0000);

	makeExe(0xa01b0000, 0xa01b0000, 0x1000, 0, 0, 0);
	CHECK_RESULT(planEmbeddedApp(exeBuffer, 0, &plan), APP_PLAN_OK);
	CHECK(plan.dest == 0x801b0000);
}

int main(void) {
	testStandaloneSIOLoader();
	testUniROM();
	testCDLoader();
	testOrdinaryHomebrew();
	testArenaAvoidsSourceBlob();
	testArenaClearsDashboardImage();
	testEraseRamFills();
	testRejections();
	testBssPushesArenaAside();
	testPhysicalAddressesAreCanonicalised();

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
