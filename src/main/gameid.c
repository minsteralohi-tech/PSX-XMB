/*
 * PSX-iTests - disc game ID detection
 *
 * Reads the boot executable name out of SYSTEM.CNF on an inserted disc and
 * reports it as a game ID, e.g. "SLUS_004.02" for Tekken 3.
 *
 * WHAT THIS IS AND IS NOT
 * -----------------------
 * jdfr228's PS1-Disc-Based-Game-ID is a set of BIOS patches: they hook the
 * kernel's parseConfig() and push the ID out over SIO0 to a MemCard Pro or
 * SD2PSX, so that a *game* running from disc gets a per-title memory card
 * page. That needs a replacement BIOS chip and only helps hardware that
 * listens on the memory card bus.
 *
 * The dashboard does not need any of that. It is already running, so it can
 * simply read SYSTEM.CNF itself and show what it finds. The extraction rule
 * here is deliberately the same one the BIOS patch uses - take the boot path
 * and cut it at the ';' before the version suffix - so both report an
 * identical string for the same disc.
 *
 * HOW IT WORKS
 * ------------
 *   1. Poll the drive's shell-open flag every frame (cheap: one GetStat).
 *   2. On an open -> closed transition, wait for the drive to settle, then
 *      read ISO9660 sector 16 (the Primary Volume Descriptor), follow it to
 *      the root directory, scan for SYSTEM.CNF, read its first sector and
 *      pull the BOOT= line out of it.
 *   3. Hand the result to the UI, which shows it briefly in the corner.
 *
 * The read only happens on that transition, never per frame, because it does
 * block for a few hundred milliseconds. Every wait is bounded - a drive that
 * never becomes ready must not freeze the dashboard.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "main/font.h"
#include "main/gameid.h"
#include "main/renderer.h"
#include "main/xmb_bg.h"

#define CDROM_REG0 (*(volatile uint8_t *) 0x1f801800)
#define CDROM_REG1 (*(volatile uint8_t *) 0x1f801801)
#define CDROM_REG2 (*(volatile uint8_t *) 0x1f801802)
#define CDROM_REG3 (*(volatile uint8_t *) 0x1f801803)

#define STAT_BUSYSTS 0x80
#define STAT_RSLRRDY 0x20
#define STAT_DRQSTS  0x40

/* Bits in the drive status byte returned by GetStat. */
#define DRIVE_SHELL_OPEN 0x10

#define CMD_GETSTAT 0x01
#define CMD_SETLOC  0x02
#define CMD_READN   0x06
#define CMD_PAUSE   0x09
#define CMD_INIT    0x0a
#define CMD_SETMODE 0x0e

#define SECTOR_SIZE   2048
#define PVD_SECTOR    16

/* Spin budgets. Generous enough for a real drive, finite so a missing or
 * faulty one degrades to "no ID" instead of a hang. */
#define GUARD_SHORT 0x80000u
#define GUARD_LONG  0x800000u

static GameIdState current;

/* --- game name database ------------------------------------------------- *
 *
 * Built by tools/makeGameDb.py from assets/PSX_ID.txt; see that file for the
 * layout. Keys pack a prefix index and the disc number into 32 bits, so a
 * lookup is an integer binary search over 1404 entries - about 11 probes,
 * with no string compares and no ID text stored at all.
 */
extern const uint8_t gameDbData[];

#define DB_PREFIX_COUNT 8

static const char *const dbPrefixes[DB_PREFIX_COUNT] = {
	"SLUS", "SCUS", "SLES", "SCES", "SLPS", "SCPS", "SLPM", "SCAJ"
};

static uint16_t dbRead16(const uint8_t *p) {
	return (uint16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8));
}

static uint32_t dbRead32(const uint8_t *p) {
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
	       ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

/*
 * Turn a boot name such as "SLUS_004.02" into the packed key the table uses.
 * Returns false for anything that is not <4 letters><separator><5 digits>.
 */
static bool dbPackKey(const char *bootName, uint32_t *key) {
	char prefix[5];
	int  i;

	for (i = 0; i < 4; i++) {
		char c = bootName[i];

		if (c >= 'a' && c <= 'z')
			c = (char) (c - 'a' + 'A');

		if (c < 'A' || c > 'Z')
			return false;

		prefix[i] = c;
	}

	prefix[4] = '\0';

	int index = -1;

	for (i = 0; i < DB_PREFIX_COUNT; i++) {
		const char *p = dbPrefixes[i];

		if (p[0] == prefix[0] && p[1] == prefix[1] &&
		    p[2] == prefix[2] && p[3] == prefix[3]) {
			index = i;
			break;
		}
	}

	if (index < 0)
		return false;

	// Collect the digits, ignoring the '_', '-' and '.' separators that the
	// boot name and the printed ID use differently: SLUS_004.02 and
	// SLUS-00402 are the same disc.
	uint32_t number = 0;
	int      digits = 0;

	for (i = 4; bootName[i] && digits < 5; i++) {
		char c = bootName[i];

		if (c == '_' || c == '-' || c == '.')
			continue;

		if (c < '0' || c > '9')
			return false;

		number = number * 10 + (uint32_t) (c - '0');
		digits++;
	}

	if (digits != 5)
		return false;

	*key = ((uint32_t) index << 27) | (number & 0x07ffffffu);
	return true;
}

/* Look up a boot name. Returns the game name, or NULL if not in the table. */
static const char *dbLookup(const char *bootName) {
	if (gameDbData[0] != 'P' || gameDbData[1] != 'S' ||
	    gameDbData[2] != 'X' || gameDbData[3] != 'G')
		return NULL;

	uint32_t key;

	if (!dbPackKey(bootName, &key))
		return NULL;

	int count = (int) dbRead16(&gameDbData[4]);

	if (count <= 0)
		return NULL;

	const uint8_t *table = &gameDbData[8];
	const uint8_t *blob  = table + (unsigned) count * 6u;

	int lo = 0, hi = count - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		const uint8_t *entry = table + (unsigned) mid * 6u;
		uint32_t probe = dbRead32(entry);

		if (probe == key)
			return (const char *) (blob + dbRead16(entry + 4));

		if (probe < key)
			lo = mid + 1;
		else
			hi = mid - 1;
	}

	return NULL;
}

/* --- low level ---------------------------------------------------------- */

static void cdAckAll(void) {
	CDROM_REG0 = 1;
	CDROM_REG3 = 0x1f;
	CDROM_REG3 = 0x40;
	CDROM_REG0 = 0;
}

/* Returns the interrupt type (1..5), or 0 if nothing arrived in time. */
static int cdWaitInt(unsigned guard) {
	for (;;) {
		CDROM_REG0 = 1;
		uint8_t flags = CDROM_REG3 & 0x07;
		CDROM_REG0 = 0;

		if (flags)
			return flags;

		if (!--guard)
			return 0;
	}
}

/*
 * Issue a command and collect its first response. Returns the interrupt type,
 * 0 on timeout. respLen may be NULL.
 */
static int cdCommand(
	uint8_t cmd, const uint8_t *params, int paramCount,
	uint8_t *resp, int respMax, int *respLen
) {
	unsigned guard = GUARD_SHORT;

	while ((CDROM_REG0 & STAT_BUSYSTS) && --guard)
		;

	if (!guard)
		return 0;

	cdAckAll();

	CDROM_REG0 = 0;
	for (int i = 0; i < paramCount; i++)
		CDROM_REG2 = params[i];

	CDROM_REG1 = cmd;

	int type = cdWaitInt(GUARD_LONG);
	int n    = 0;

	if (type) {
		while ((CDROM_REG0 & STAT_RSLRRDY) && n < respMax)
			resp[n++] = CDROM_REG1;
	}

	CDROM_REG0 = 1;
	CDROM_REG3 = 0x1f;
	CDROM_REG0 = 0;

	if (respLen)
		*respLen = n;

	return type;
}

/* Drive status byte, or 0xff if the drive did not answer. */
static uint8_t cdGetStat(void) {
	uint8_t resp[8];
	int     len = 0;

	if (!cdCommand(CMD_GETSTAT, NULL, 0, resp, sizeof(resp), &len) || !len)
		return 0xff;

	return resp[0];
}

static void decToBcd(int value, uint8_t *out) {
	*out = (uint8_t) (((value / 10) << 4) | (value % 10));
}

/*
 * Read one 2048-byte sector by LBA. Returns true on success.
 *
 * LBA 0 is at 00:02:00 on a Mode 2 disc, so the minute/second/frame the drive
 * wants is the LBA plus 150 frames.
 */
static bool cdReadSector(uint32_t lba, uint8_t *out) {
	uint32_t amount = lba + 150;
	uint8_t  loc[3];

	decToBcd((int) (amount / (60 * 75)), &loc[0]);
	decToBcd((int) ((amount / 75) % 60), &loc[1]);
	decToBcd((int) (amount % 75),        &loc[2]);

	uint8_t resp[8];

	if (!cdCommand(CMD_SETLOC, loc, 3, resp, sizeof(resp), NULL))
		return false;

	/* Mode: 2048-byte sectors, double speed. */
	uint8_t mode = 0x80;

	if (!cdCommand(CMD_SETMODE, &mode, 1, resp, sizeof(resp), NULL))
		return false;

	if (!cdCommand(CMD_READN, NULL, 0, resp, sizeof(resp), NULL))
		return false;

	/* Wait for the data-ready interrupt (INT1). */
	int type = cdWaitInt(GUARD_LONG);

	if (type != 1) {
		cdAckAll();
		cdCommand(CMD_PAUSE, NULL, 0, resp, sizeof(resp), NULL);
		return false;
	}

	/* Drain the response FIFO before touching the data FIFO. */
	while (CDROM_REG0 & STAT_RSLRRDY)
		(void) CDROM_REG1;

	CDROM_REG0 = 1;
	CDROM_REG3 = 0x07;      /* acknowledge INT1 */
	CDROM_REG0 = 0;

	/* Request the sector into the data FIFO, then read it a byte at a time.
	 * Deliberately not DMA: this runs while the dashboard owns the DMA
	 * channels, and 2 KB of PIO once per disc insertion is not worth the
	 * risk of disturbing them. */
	CDROM_REG0 = 0;
	CDROM_REG3 = 0x80;

	unsigned guard = GUARD_SHORT;

	while (!(CDROM_REG0 & STAT_DRQSTS) && --guard)
		;

	if (!guard) {
		cdCommand(CMD_PAUSE, NULL, 0, resp, sizeof(resp), NULL);
		return false;
	}

	for (int i = 0; i < SECTOR_SIZE; i++)
		out[i] = CDROM_REG2;

	CDROM_REG0 = 0;
	CDROM_REG3 = 0x00;      /* stop requesting data */

	cdCommand(CMD_PAUSE, NULL, 0, resp, sizeof(resp), NULL);
	cdAckAll();

	return true;
}

/* --- ISO9660 ------------------------------------------------------------ */

static uint32_t readLE32(const uint8_t *p) {
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
	       ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static bool nameMatches(const uint8_t *name, int len, const char *want) {
	int i = 0;

	for (; i < len && want[i]; i++) {
		uint8_t c = name[i];

		if (c >= 'a' && c <= 'z')
			c = (uint8_t) (c - 'a' + 'A');

		if (c != (uint8_t) want[i])
			return false;
	}

	if (!want[i])
		return true;   /* matched the whole wanted name */

	return false;
}

/*
 * Find SYSTEM.CNF in the root directory. Returns its LBA, or 0.
 *
 * Only the first sector of the root directory is scanned. SYSTEM.CNF is
 * always among the first entries on a real PS1 disc - the BIOS itself relies
 * on that - so walking multi-sector directories is not worth the code.
 */
static uint32_t findSystemCnf(uint8_t *scratch) {
	if (!cdReadSector(PVD_SECTOR, scratch))
		return 0;

	if (scratch[0] != 0x01 ||
	    scratch[1] != 'C' || scratch[2] != 'D' ||
	    scratch[3] != '0' || scratch[4] != '0' || scratch[5] != '1')
		return 0;   /* not an ISO9660 primary volume descriptor */

	/* Root directory record lives at offset 156; its extent LBA at +2. */
	uint32_t rootLba = readLE32(&scratch[156 + 2]);

	if (!rootLba)
		return 0;

	if (!cdReadSector(rootLba, scratch))
		return 0;

	int offset = 0;

	while (offset < SECTOR_SIZE) {
		int recLen = scratch[offset];

		if (recLen < 33)
			break;   /* end of the records in this sector */

		if (offset + recLen > SECTOR_SIZE)
			break;

		int nameLen = scratch[offset + 32];

		if (nameLen > 0 &&
		    nameMatches(&scratch[offset + 33], nameLen, "SYSTEM.CNF"))
			return readLE32(&scratch[offset + 2]);

		offset += recLen;
	}

	return 0;
}

/*
 * Pull the game ID out of a SYSTEM.CNF sector.
 *
 * The file looks like:
 *     BOOT = cdrom:\SLUS_004.02;1
 *     TCB = 4
 *     ...
 *
 * Take what follows the last '\' or ':' on the BOOT line and stop at ';',
 * which is exactly what the BIOS patch's IDSTRINGBUILDER loop does.
 */
static bool parseBootId(const uint8_t *sector, char *out, int outSize) {
	for (int i = 0; i + 4 < SECTOR_SIZE; i++) {
		if (sector[i] != 'B' || sector[i + 1] != 'O' ||
		    sector[i + 2] != 'O' || sector[i + 3] != 'T')
			continue;

		int j = i + 4;

		/* Skip spaces and the '=' */
		while (j < SECTOR_SIZE &&
		       (sector[j] == ' ' || sector[j] == '\t' || sector[j] == '='))
			j++;

		/* Walk to the end of the token, remembering the last separator. */
		int start = j;

		while (j < SECTOR_SIZE && sector[j] > ' ' && sector[j] != ';') {
			if (sector[j] == '\\' || sector[j] == '/' || sector[j] == ':')
				start = j + 1;
			j++;
		}

		int len = j - start;

		if (len <= 0 || len >= outSize)
			return false;

		for (int k = 0; k < len; k++) {
			uint8_t c = sector[start + k];

			if (c >= 'a' && c <= 'z')
				c = (uint8_t) (c - 'a' + 'A');

			out[k] = (char) c;
		}

		out[len] = '\0';
		return true;
	}

	return false;
}

/* --- public ------------------------------------------------------------- */

void gameIdInit(void) {
	current.state      = GAMEID_IDLE;
	current.id[0]      = '\0';
	current.noticeTime = 0;
}

const GameIdState *gameIdGet(void) {
	return &current;
}

void gameIdClearNotice(void) {
	current.noticeTime = 0;
}

/*
 * Read the disc now. Blocking, a few hundred milliseconds at worst, all of it
 * bounded. Called from gameIdPoll() on a lid-close transition.
 */
static void gameIdReadDisc(uint8_t *scratch) {
	uint32_t lba = findSystemCnf(scratch);

	if (!lba) {
		current.state = GAMEID_NO_DISC;
		current.id[0] = '\0';
		return;
	}

	char bootName[32];

	if (!cdReadSector(lba, scratch) ||
	    !parseBootId(scratch, bootName, sizeof(bootName))) {
		current.state = GAMEID_UNKNOWN;
		current.id[0] = '\0';
		return;
	}

	// Only the game name is interesting on screen - "SLUS-00402" tells the
	// user nothing. A disc that is not in the table falls back to the raw
	// boot name, which at least identifies an import or a homebrew disc.
	const char *name = dbLookup(bootName);

	if (name) {
		int i = 0;

		while (name[i] && i < (int) sizeof(current.id) - 1) {
			current.id[i] = name[i];
			i++;
		}

		current.id[i]  = '\0';
		current.state  = GAMEID_FOUND;
		return;
	}

	int i = 0;

	while (bootName[i] && i < (int) sizeof(current.id) - 1) {
		current.id[i] = bootName[i];
		i++;
	}

	current.id[i] = '\0';
	current.state = GAMEID_UNLISTED;
}

void gameIdPoll(uint8_t *scratch, int scratchSize) {
	static bool wasOpen   = false;
	static int  settle    = 0;
	static int  pollDelay = 0;

	if (current.noticeTime > 0)
		current.noticeTime--;

	if (scratchSize < SECTOR_SIZE)
		return;

	/* One GetStat per ~15 frames is plenty to notice a lid, and keeps this
	 * off the per-frame path entirely most of the time. */
	if (settle > 0) {
		settle--;

		if (settle == 0) {
			gameIdReadDisc(scratch);
			current.noticeTime = GAMEID_NOTICE_FRAMES;
		}

		return;
	}

	if (pollDelay > 0) {
		pollDelay--;
		return;
	}

	pollDelay = 15;

	uint8_t stat = cdGetStat();

	if (stat == 0xff)
		return;

	bool isOpen = (stat & DRIVE_SHELL_OPEN) != 0;

	if (isOpen && !wasOpen) {
		/* Lid opened: forget whatever was in the drive. */
		current.state = GAMEID_IDLE;
		current.id[0] = '\0';
	}

	if (!isOpen && wasOpen) {
		/*
		 * Lid closed. Give the drive time to spin up and get a table of
		 * contents before asking it for sectors - reading immediately after
		 * the shell closes fails on real hardware. Roughly two seconds.
		 */
		settle = 120;
	}

	wasOpen = isOpen;
}


/* --- corner notification ------------------------------------------------ */

static uint32_t shade(uint32_t colour, int numerator, int denominator) {
	uint32_t r = ((colour        & 0xff) * numerator) / denominator;
	uint32_t g = (((colour >> 8)  & 0xff) * numerator) / denominator;
	uint32_t b = (((colour >> 16) & 0xff) * numerator) / denominator;

	if (r > 0xff) r = 0xff;
	if (g > 0xff) g = 0xff;
	if (b > 0xff) b = 0xff;

	return (b << 16) | (g << 8) | r;
}

/*
 * Slide-in notification, PS4/PS5 style.
 *
 * One line, the game name only - the raw ID told the user nothing. It eases
 * in from off the right edge, holds, then eases back out, so it never steals
 * focus from whatever screen is up.
 *
 * slideFx is 0..256 (fully out .. fully in) and is driven here rather than in
 * gameIdPoll(), because the animation should run at frame rate while the poll
 * deliberately does not.
 */
void drawGameIdNotice(RenderContext *ctx) {
	static int slideFx = 0;

	const char *value;

	switch (current.state) {
	case GAMEID_FOUND:
	case GAMEID_UNLISTED:
		value = current.id;
		break;

	case GAMEID_NO_DISC:
		value = "Disc not readable";
		break;

	case GAMEID_UNKNOWN:
		value = "Unknown disc";
		break;

	default:
		value = NULL;
		break;
	}

	// Ease toward fully in while the notice is live and fully out after,
	// with the same >> 3 filter the XMB menu uses for its own glides.
	int target = (value && current.noticeTime > 0) ? 256 : 0;

	slideFx += (target - slideFx) >> 3;

	// >> 3 never quite reaches the target; snap the last pixel so the card
	// actually leaves the screen instead of parking one step short.
	if (target == 0 && slideFx < 4)
		slideFx = 0;
	if (target == 256 && slideFx > 252)
		slideFx = 256;

	if (!slideFx || !value)
		return;

	int textW = getStringWidth(value);
	int boxW  = textW + 18;

	if (boxW > 260)
		boxW = 260;

	int boxH = 22;                       /* one line, slim */
	int boxY = 16;

	// Fully in: 16px from the right edge. Fully out: just past it.
	int restX = 304 - boxW;
	int boxX  = restX + ((320 - restX) * (256 - slideFx)) / 256;

	uint32_t accent;

	xmbGetAccentColor(&accent, NULL);

	uint32_t body = shade(accent, 3, 4);

	/* Twice, for the same ~75% opacity the dialogs use. */
	drawRect(ctx, boxX,     boxY + 1,        boxW,     boxH - 2, body, true);
	drawRect(ctx, boxX,     boxY + 1,        boxW,     boxH - 2, body, true);
	drawRect(ctx, boxX + 1, boxY,            boxW - 2, 1,        body, true);
	drawRect(ctx, boxX + 1, boxY + boxH - 1, boxW - 2, 1,        body, true);

	uint32_t lit = shade(accent, 5, 2);
	uint32_t dim = shade(accent, 1, 3);

	drawRect(ctx, boxX + 1,        boxY,            boxW - 2, 1,        lit, true);
	drawRect(ctx, boxX,            boxY + 1,        1,        boxH - 2, lit, true);
	drawRect(ctx, boxX + 1,        boxY + boxH - 1, boxW - 2, 1,        dim, true);
	drawRect(ctx, boxX + boxW - 1, boxY + 1,        1,        boxH - 2, dim, true);

	printString(ctx, boxX + 9, boxY + 7, 0xffffff, value);
}
