/*
 * PSX-iTests - generic two-stage launcher (see app_launch.h).
 */

#include <stdint.h>
#include "ps1/cache.h"
#include "main/app_launch.h"
#include "main/handoff.h"

/*
 * End of everything the dashboard occupies, from cmake/executable.ld.
 *
 * APP_LAUNCH_HOST_TEST lets tools/test_app_launch.c compile this same file on
 * a PC and vary the dashboard's footprint, which is the one input the arena
 * choice depends on and the one that changes every time an asset is added.
 * Only the planner is built in that mode; the hardware handoff below is not.
 */
#ifdef APP_LAUNCH_HOST_TEST
uint32_t appTestImageEnd = 0x801a6000u;
#define DASHBOARD_IMAGE_END (appTestImageEnd)
#else
extern uint8_t _imageEnd[];
#define DASHBOARD_IMAGE_END ((uint32_t) _imageEnd)
#endif

/*
 * Arena candidates, tried in order. All but the last are above the
 * dashboard's image, so installing stage 1 cannot clobber a global the
 * dashboard is still using between the install and the jump.
 *
 *   0x801ff000  top 4 KB of RAM. Clear for the standalone SIO loader
 *               (0x801b0000-0x801c0000) and for UniROM (ends 0x801f1000).
 *               A target whose stack lives here is fine - stage 1 has
 *               already jumped by the time the target pushes anything.
 *   0x801e0000  for targets that want the very top, e.g. cdloader.exe at
 *               0x801ea300.
 *   0x801c8000  for targets that want both of the above.
 *   0x8000c000  BIOS kernel scratch. Last resort only: nothing documents it
 *               as free, and the kernel's event/thread control blocks and a
 *               resident cheat cartridge's hooks live in that 64 KB. It is
 *               here so a target that wants all of 0x801c8000-0x80200000
 *               still has somewhere to put stage 1.
 */
static const uint32_t ARENA_CANDIDATES[] = {
	0x801ff000u,
	0x801e0000u,
	0x801c8000u,
	0x8000c000u,
};

#define ARENA_CANDIDATE_COUNT \
	(sizeof(ARENA_CANDIDATES) / sizeof(ARENA_CANDIDATES[0]))

static int rangesOverlap(uint32_t aStart, uint32_t aSize,
                         uint32_t bStart, uint32_t bSize) {
	if (!aSize || !bSize)
		return 0;

	uint32_t aEnd = aStart + aSize;
	uint32_t bEnd = bStart + bSize;

	if (aEnd < aStart || bEnd < bStart)
		return 1;   /* a wrapping range is treated as hitting everything */

	return (aStart < bEnd) && (bStart < aEnd);
}

static int addFill(AppLaunchPlan *plan, uint32_t start, uint32_t bytes) {
	if (!bytes)
		return 1;

	if (plan->fillCount >= APP_MAX_FILLS)
		return 0;

	plan->fillStart[plan->fillCount] = start;
	plan->fillBytes[plan->fillCount] = bytes;
	plan->fillCount++;
	return 1;
}

/*
 * Add [start, end) as a zero-fill, minus the arena. The arena holds stage 1,
 * its parameters and this very fill list while the fills are running, so it
 * has to survive; punching it out can split one range into two.
 */
static int addFillExcludingArena(AppLaunchPlan *plan,
                                 uint32_t start, uint32_t end) {
	if (end <= start)
		return 1;

	uint32_t arenaStart = plan->arena;
	uint32_t arenaEnd   = plan->arena + APP_ARENA_SIZE;

	if (arenaEnd <= start || arenaStart >= end)
		return addFill(plan, start, end - start);

	int ok = 1;

	if (arenaStart > start)
		ok = ok && addFill(plan, start, arenaStart - start);

	if (arenaEnd < end)
		ok = ok && addFill(plan, arenaEnd, end - arenaEnd);

	return ok;
}

AppPlanResult planEmbeddedApp(const uint8_t *exe, int eraseRam,
                              AppLaunchPlan *plan) {
	const PSEXEHeader *hdr = (const PSEXEHeader *) exe;

	for (unsigned i = 0; i < sizeof(*plan); i++)
		((uint8_t *) plan)[i] = 0;

	const char *magic = "PS-X EXE";

	for (int i = 0; i < 8; i++) {
		if (hdr->magic[i] != magic[i])
			return APP_PLAN_BAD_MAGIC;
	}

	uint32_t dest  = hdr->textAddr & 0x1fffffffu;
	uint32_t entry = hdr->pc       & 0x1fffffffu;

	dest  |= 0x80000000u;
	entry |= 0x80000000u;

	uint32_t bodySize = hdr->textSize;

	if (!bodySize)
		return APP_PLAN_BAD_SIZE;

	if ((dest & 3u) || (entry & 3u))
		return APP_PLAN_UNALIGNED;

	uint32_t destEnd = dest + bodySize;

	if (destEnd < dest || dest < APP_RAM_BASE || destEnd > APP_RAM_TOP)
		return APP_PLAN_DEST_RANGE;

	if (entry < dest || entry >= destEnd)
		return APP_PLAN_ENTRY_RANGE;

	uint32_t sp = hdr->spBase
	            ? ((hdr->spBase + hdr->spOffset) | 0x80000000u)
	            : 0x801fff00u;

	if (sp & 3u)
		return APP_PLAN_UNALIGNED;

	if (sp <= APP_RAM_BASE || sp > APP_RAM_TOP)
		return APP_PLAN_SP_RANGE;

	uint32_t bssStart = 0, bssBytes = 0;

	if (hdr->bssSize) {
		bssStart = (hdr->bssAddr & 0x1fffffffu) | 0x80000000u;
		bssBytes = (hdr->bssSize + 3u) & ~3u;

		if (bssStart & 3u)
			return APP_PLAN_UNALIGNED;

		if (bssBytes < hdr->bssSize ||
		    bssStart < APP_RAM_BASE ||
		    bssStart + bssBytes > APP_RAM_TOP ||
		    bssStart + bssBytes < bssStart)
			return APP_PLAN_BSS_RANGE;
	}

	uint32_t src      = (uint32_t) (uintptr_t) (exe + PSEXE_PAYLOAD_OFFSET);
	uint32_t imageEnd = DASHBOARD_IMAGE_END;

	plan->entry    = entry;
	plan->gp       = hdr->gp;
	plan->sp       = sp;
	plan->dest     = dest;
	plan->destEnd  = destEnd;
	plan->bodySize = bodySize;
	plan->src      = src;
	plan->imageEnd = imageEnd;

	/*
	 * Pick the first arena that clears the destination, the source blob and
	 * the target's BSS. Candidates inside main RAM must also be at or above
	 * the dashboard's _imageEnd; the kernel-scratch fallback is below the
	 * dashboard entirely and so is exempt.
	 */
	plan->arena = 0;

	for (unsigned i = 0; i < ARENA_CANDIDATE_COUNT; i++) {
		uint32_t candidate = ARENA_CANDIDATES[i];

		if (candidate >= APP_RAM_BASE && candidate < imageEnd)
			continue;

		if (rangesOverlap(candidate, APP_ARENA_SIZE, dest, bodySize))
			continue;

		if (rangesOverlap(candidate, APP_ARENA_SIZE, src, bodySize))
			continue;

		if (bssBytes &&
		    rangesOverlap(candidate, APP_ARENA_SIZE, bssStart, bssBytes))
			continue;

		plan->arena = candidate;
		break;
	}

	if (!plan->arena)
		return APP_PLAN_NO_ARENA;

	/*
	 * Zero-fill list. Order matters only in that the payload copy happens
	 * before any of it; stage 1 guarantees that. The destination itself is
	 * never in the list, and the arena is punched out of every range.
	 */
	int ok = 1;

	if (eraseRam) {
		ok = ok && addFillExcludingArena(plan, APP_RAM_BASE, dest);
		ok = ok && addFillExcludingArena(plan, destEnd, APP_RAM_TOP);
	}

	/*
	 * PS-EXE memfill, applied last so it wins over anything above. The
	 * arena was already proven clear of it, so it goes in as-is.
	 */
	ok = ok && addFill(plan, bssStart, bssBytes);

	if (!ok)
		return APP_PLAN_TOO_MANY_FILLS;

	return APP_PLAN_OK;
}

const char *appPlanResultText(AppPlanResult result) {
	switch (result) {
	case APP_PLAN_OK:              return "ok";
	case APP_PLAN_BAD_MAGIC:       return "not a PS-EXE";
	case APP_PLAN_BAD_SIZE:        return "bad payload size";
	case APP_PLAN_UNALIGNED:       return "address not word aligned";
	case APP_PLAN_DEST_RANGE:      return "load address outside usable RAM";
	case APP_PLAN_ENTRY_RANGE:     return "entry point outside image";
	case APP_PLAN_SP_RANGE:        return "stack pointer outside usable RAM";
	case APP_PLAN_BSS_RANGE:       return "BSS outside usable RAM";
	case APP_PLAN_NO_ARENA:        return "no free arena for the trampoline";
	case APP_PLAN_TOO_MANY_FILLS:  return "fill list overflow";
	}

	return "unknown";
}

#ifndef APP_LAUNCH_HOST_TEST

__attribute__((noreturn)) void runAppLaunch(const AppLaunchPlan *plan) {
	/* Copy the plan into locals first: everything below runs after the
	 * hardware has been quiesced, and the plan itself may live in .bss that
	 * the launched program is about to inherit. */
	const uint32_t arena     = plan->arena;
	const uint32_t dest      = plan->dest;
	const uint32_t src       = plan->src;
	const uint32_t copyWords = (plan->bodySize + 3u) / 4u;
	const uint32_t entry     = plan->entry;
	const uint32_t gp        = plan->gp;
	const uint32_t sp        = plan->sp;
	const uint32_t fillCount = plan->fillCount;

	uint32_t fillStart[APP_MAX_FILLS];
	uint32_t fillWords[APP_MAX_FILLS];

	for (uint32_t i = 0; i < fillCount; i++) {
		fillStart[i] = plan->fillStart[i];
		fillWords[i] = (plan->fillBytes[i] + 3u) / 4u;
	}

	quiesceForHandoff();

	/* Stage 1 code. */
	volatile uint32_t *code  = (volatile uint32_t *) arena;
	const uint32_t stubWords = (uint32_t) (appStubEnd - appStubStart);

	for (uint32_t i = 0; i < stubWords; i++)
		code[i] = appStubStart[i];

	/* Zero-fill descriptor list, terminated by a zero word count. */
	volatile uint32_t *fills =
		(volatile uint32_t *) (arena + APP_ARENA_FILL_OFF);

	for (uint32_t i = 0; i < fillCount; i++) {
		fills[i * 2 + 0] = fillStart[i];
		fills[i * 2 + 1] = fillWords[i];
	}

	fills[fillCount * 2 + 0] = 0;
	fills[fillCount * 2 + 1] = 0;

	/* Parameter block. */
	volatile uint32_t *params =
		(volatile uint32_t *) (arena + APP_ARENA_PARAM_OFF);

	params[0] = dest;
	params[1] = src;
	params[2] = copyWords;
	params[3] = entry;
	params[4] = gp;
	params[5] = sp;
	params[6] = arena + APP_ARENA_FILL_OFF;
	params[7] = 0;

	/* Stage 1 was just written as data. */
	flushCache();

	/*
	 * Enter stage 1 with its parameter pointer in $t8 and its own address in
	 * $t9. It reads no absolute addresses, so the arena choice above is the
	 * only thing that decides where it runs.
	 */
	__asm__ volatile(
		".set push\n"
		".set noreorder\n"
		"move $t8, %0\n"
		"move $t9, %1\n"
		"jr   $t9\n"
		"nop\n"
		".set pop\n"
		:
		: "r"(params), "r"(code)
		: "$t8", "$t9", "memory"
	);

	__builtin_unreachable();
}

AppPlanResult launchEmbeddedApp(const uint8_t *exe, int eraseRam) {
	AppLaunchPlan plan;
	AppPlanResult result = planEmbeddedApp(exe, eraseRam, &plan);

	if (result != APP_PLAN_OK)
		return result;

	runAppLaunch(&plan);
}

#endif /* !APP_LAUNCH_HOST_TEST */
