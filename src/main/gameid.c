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
 * READING THE DISC
 * -----------------
 * Three earlier attempts crashed or failed on real hardware. The UniROM 8.0.K
 * disassembly explains both mistakes, and this version follows what that
 * driver actually does:
 *
 *   1. MASK THE CD INTERRUPT WHILE WE DRIVE THE CONTROLLER. The retail BIOS
 *      keeps its own CD-ROM interrupt handler live. Polling the interrupt
 *      flag register and acknowledging flags while that handler can also fire
 *      is what crashed the dashboard: it sees flags cleared that it was about
 *      to act on. UniROM disables interrupts around its transfers for the
 *      same reason. Here the CD bit is cleared in IRQ_MASK for the duration
 *      and restored afterwards, so the BIOS handler simply never runs while
 *      we are talking to the drive.
 *
 *   2. DRAIN THE RESULT FIFO AFTER EVERY COMMAND. The disassembly's
 *      multi-result collector reads result bytes while RSLRRDY stays set,
 *      before the next command. Issuing a command with bytes still pending
 *      desynchronises the controller, which is the classic "works on an
 *      emulator, fails on hardware" difference - emulators are forgiving
 *      about it and real silicon is not.
 *
 * The read sequence is UniROM's: Setmode(0) -> Setloc -> wait for INT3, with
 * a bounded retry that pauses and re-seeks -> ReadN -> per sector, wait for
 * the data-ready interrupt and take the sector -> Pause.
 *
 * Sectors are taken through the data port rather than DMA channel 3. UniROM
 * uses DMA, but the dashboard owns those channels for the GPU and one 2 KB
 * transfer per scan is not worth the risk of disturbing them.
 *
 * HOW IT WORKS * HOW IT WORKS
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
#include "main/defs.h"
#include "main/font.h"
#include "main/sound.h"
#include "main/gameid.h"
#include "main/renderer.h"
#include "main/xmb_bg.h"

static GameIdState current;

/* Chime whenever the card appears, including the "Reading disc..." step so
 * the user gets feedback the instant the drive is touched. */
static void gameIdNoticeSound(void) {
	playNotifySound();
}

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

/*
 * SYSTEM.CNF is read through the BIOS's own "cdrom:" filesystem device rather
 * than by driving the CD-ROM registers directly.
 *
 * The first version of this file did drive them directly - SetLoc/SetMode/
 * ReadN, then pulling the sector out of the data FIFO by hand, plus its own
 * ISO9660 directory walk. That worked on emulators and failed on every real
 * console, which is the classic signature: emulators are forgiving about
 * command timing, the INT1 handshake and the DRQ/data-FIFO protocol, and real
 * hardware is not. It also stalled for seconds on disc insertion, because a
 * failing read sat in its retry loops.
 *
 * The BIOS already implements all of that correctly, with spin-up handling and
 * retries, and it is what cdloader and tonyhax use for the same job. _96_init()
 * brings the device up; after that a "cdrom:" path works with the ordinary
 * open()/read()/close() calls.
 */

#include "ps1/registers.h"

#define CDREG0 (*(volatile uint8_t *) 0xbf801800)
#define CDREG1 (*(volatile uint8_t *) 0xbf801801)
#define CDREG2 (*(volatile uint8_t *) 0xbf801802)
#define CDREG3 (*(volatile uint8_t *) 0xbf801803)

#define ST_RSLRRDY 0x20
#define ST_DRQSTS  0x40
#define ST_BUSYSTS 0x80

#define CMD_SETLOC  0x02
#define CMD_READN   0x06
#define CMD_PAUSE   0x09
#define CMD_SETMODE 0x0e
#define CMD_STOP    0x08

/*
 * Spin budgets.
 *
 * Deliberately modest. These are how long a FAILING operation takes, and a
 * failure is the common case while a drive is still spinning up - or forever,
 * on an audio CD, which has no data track to read at all. The original values
 * were roughly 8x larger and made each failed attempt take long enough to
 * drop several frames; with a dozen retries that turned into seconds of
 * stutter and, on an audio disc, a dashboard that looked like it had hung.
 *
 * A drive that is genuinely ready answers well inside these.
 */
#define GUARD_CMD  0x40000u
#define GUARD_DATA 0x120000u

static uint16_t cdSavedMask;

/*
 * Take the CD-ROM away from the BIOS handler for the duration of a scan.
 * See point 1 in the header comment - without this the dashboard crashes.
 */
static void cdBegin(void) {
	cdSavedMask = IRQ_MASK;
	IRQ_MASK    = (uint16_t) (cdSavedMask & ~(1 << IRQ_CDROM));
}

static void cdEnd(void) {
	IRQ_STAT = (uint16_t) ~(1 << IRQ_CDROM);
	IRQ_MASK = cdSavedMask;
}

/* Read the interrupt flag, 0 if none within the budget. */
static int cdWaitInt(unsigned guard) {
	for (;;) {
		CDREG0 = 1;
		uint8_t flags = CDREG3 & 0x07;
		CDREG0 = 0;

		if (flags)
			return flags;

		if (!--guard)
			return 0;
	}
}

/* Result bytes from the last command, for GetStat. */
static uint8_t cdResult[8];
static int     cdResultLen;

/* Drain every pending result byte, then acknowledge. Point 2 above. */
static void cdDrainAndAck(void) {
	CDREG0 = 0;

	unsigned guard = 64;

	cdResultLen = 0;

	while ((CDREG0 & ST_RSLRRDY) && --guard) {
		uint8_t b = CDREG1;

		if (cdResultLen < (int) sizeof(cdResult))
			cdResult[cdResultLen++] = b;
	}

	CDREG0 = 1;
	CDREG3 = 0x1f;      /* ack INT1..INT5      */
	CDREG3 = 0x40;      /* reset parameter FIFO */
	CDREG0 = 0;
}

/* Issue a command, wait for its interrupt, drain results. Returns the
 * interrupt type, 0 on timeout. */
static int cdCommand(uint8_t cmd, const uint8_t *params, int paramCount) {
	unsigned guard = GUARD_CMD;

	while ((CDREG0 & ST_BUSYSTS) && --guard)
		;

	if (!guard)
		return 0;

	cdDrainAndAck();

	CDREG0 = 0;

	for (int i = 0; i < paramCount; i++)
		CDREG2 = params[i];

	CDREG1 = cmd;

	int type = cdWaitInt(GUARD_CMD);

	cdDrainAndAck();
	return type;
}

static uint8_t toBcd(uint32_t v) {
	return (uint8_t) (((v / 10) << 4) | (v % 10));
}

/*
 * Read `count` consecutive 2048-byte sectors. Mirrors UniROM's reader,
 * including the bounded retry that pauses and re-seeks when the drive does
 * not answer Setloc with INT3.
 */
static bool cdReadSectors(uint32_t lba, uint8_t *dest, int count) {
	uint8_t mode = 0x00;

	if (!cdCommand(CMD_SETMODE, &mode, 1))
		return false;

	/* ISO LBA 0 is MSF 00:02:00, so 150 frames of lead-in. */
	uint32_t frames = lba + 150;
	uint8_t  loc[3] = {
		toBcd(frames / 4500),
		toBcd((frames % 4500) / 75),
		toBcd(frames % 75)
	};

	/*
	 * TEN Setloc retries. Do not reduce this.
	 *
	 * This is UniROM's value and it is load-bearing on real hardware: it is
	 * how a drive that is still spinning up gets seated. Cutting it to two
	 * broke detection completely on a console while an emulator carried on
	 * working perfectly, because emulators answer Setloc immediately and a
	 * real drive does not.
	 *
	 * The audio-CD cost this was meant to address is handled by the OUTER
	 * scan loop instead - fewer attempts, spaced further apart - which does
	 * not touch the spin-up path.
	 */
	bool seated = false;

	for (int tries = 0; tries < 10 && !seated; tries++) {
		if (cdCommand(CMD_SETLOC, loc, 3) == 3) {
			seated = true;
			break;
		}

		/* Recovery: pause, re-assert the mode, try again. */
		cdCommand(CMD_PAUSE, NULL, 0);
		cdCommand(CMD_SETMODE, &mode, 1);
	}

	if (!seated)
		return false;

	if (cdCommand(CMD_READN, NULL, 0) != 3)
		return false;

	bool ok = true;

	for (int s = 0; s < count && ok; s++) {
		/* INT1 means a sector is ready. */
		if (cdWaitInt(GUARD_DATA) != 1) {
			ok = false;
			break;
		}

		CDREG0 = 0;

		unsigned guard = 64;

		while ((CDREG0 & ST_RSLRRDY) && --guard)
			(void) CDREG1;

		CDREG0 = 1;
		CDREG3 = 0x07;
		CDREG0 = 0;

		/* Request the sector into the data FIFO. */
		CDREG0 = 0;
		CDREG3 = 0x80;

		guard = GUARD_CMD;

		while (!(CDREG0 & ST_DRQSTS) && --guard)
			;

		if (!guard) {
			ok = false;
			break;
		}

		uint8_t *out = dest + s * 2048;

		for (int i = 0; i < 2048; i++)
			out[i] = CDREG2;

		CDREG0 = 0;
		CDREG3 = 0x00;
	}

	cdCommand(CMD_PAUSE, NULL, 0);
	return ok;
}

static uint32_t rd32(const uint8_t *p) {
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
	       ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

/*
 * Locate SYSTEM.CNF and read it into scratch. Returns bytes read, or 0.
 *
 * Follows UniROM's ISO path: verify the primary volume descriptor at sector
 * 16, take the root directory extent from PVD offset 0x9E, then walk the
 * records looking for the name.
 */
#define CMD_GETSTAT 0x01
#define CMD_INIT    0x0a

static int readSystemCnfOnce(uint8_t *scratch) {
	int result = 0;

	if (cdReadSectors(16, scratch, 1) &&
	    scratch[0] == 0x01 && scratch[1] == 'C' && scratch[2] == 'D' &&
	    scratch[3] == '0' && scratch[4] == '0' && scratch[5] == '1') {

		uint32_t rootLba = rd32(&scratch[0x9e]);

		if (rootLba && cdReadSectors(rootLba, scratch, 1)) {
			uint32_t fileLba = 0;
			int      offset  = 0;

			while (offset < 2048) {
				int recLen = scratch[offset];

				if (recLen < 33 || offset + recLen > 2048)
					break;

				int nameLen = scratch[offset + 32];
				const uint8_t *name = &scratch[offset + 33];

				/* "SYSTEM.CNF;1" - match the stem and ignore the version
				 * suffix, which UniROM leaves on the name too. */
				if (nameLen >= 10) {
					static const char want[] = "SYSTEM.CNF";
					bool match = true;

					for (int k = 0; k < 10; k++) {
						uint8_t c = name[k];

						if (c >= 'a' && c <= 'z')
							c = (uint8_t) (c - 'a' + 'A');

						if (c != (uint8_t) want[k]) {
							match = false;
							break;
						}
					}

					if (match) {
						fileLba = rd32(&scratch[offset + 2]);
						break;
					}
				}

				offset += recLen;
			}

			if (fileLba && cdReadSectors(fileLba, scratch, 1))
				result = 2048;
		}
	}

	return result;
}

/*
 * Read SYSTEM.CNF, retrying because the first attempt after a disc change
 * reliably fails.
 *
 * Swapping a disc leaves the controller holding state for the disc that is
 * gone: the first Setloc/ReadN answers from the old TOC and errors out, and
 * only the following attempt sees the new disc. That is exactly the reported
 * behaviour - press R1 once and get "Disc not readable", press it again and
 * the name appears.
 *
 * The fix is to make one scan do what the user was doing by hand. Init (0x0A)
 * resets the controller and makes it re-read the TOC, and the read is then
 * attempted up to three times. Returns bytes read, GAMEID_AUDIO_DISC_MARKER
 * for an audio CD, or 0.
 */
#define GAMEID_NO_DATA_DISC (-3)

static int readSystemCnf(uint8_t *scratch, int scratchSize) {
	if (scratchSize < 2048)
		return 0;

	cdBegin();

	/* Reset the controller so it re-reads the table of contents. Without
	 * this the first attempt after a swap sees the previous disc. */
	cdCommand(CMD_INIT, NULL, 0);
	cdCommand(CMD_GETSTAT, NULL, 0);

	/*
	 * No GetID gate here, deliberately.
	 *
	 * Two attempts at using GetID to pre-classify the disc both broke
	 * detection outright. The drive answers INT5 for an unlicensed disc, and
	 * every CD-R is unlicensed - so backups were rejected before the read
	 * even started. Narrowing the test to the audio flag did not help
	 * either: pressed discs stopped being detected too, so whatever those
	 * response bytes mean in practice, they are not something to gate on.
	 *
	 * The read itself is the reliable test, and it is the one that worked.
	 * Audio CDs are handled by making failure cheap rather than by trying to
	 * predict it - see the guard values and the attempt cap.
	 */

	int result = 0;

	for (int attempt = 0; attempt < 3 && result == 0; attempt++) {
		result = readSystemCnfOnce(scratch);

		if (result == 0) {
			/* Nudge the drive and let it settle before trying again. */
			cdCommand(CMD_PAUSE, NULL, 0);
			cdCommand(CMD_GETSTAT, NULL, 0);
		}
	}

	/*
	 * Audio CD detection used to live here, using GetTN when no ISO volume
	 * was found. It was removed: on real hardware it made the dashboard
	 * stutter badly and sometimes took it down entirely. GetTN on a disc the
	 * controller has not finished identifying is evidently not safe to issue
	 * from this path, and an audio disc is something the CD player already
	 * handles properly.
	 */

	cdEnd();
	return result;
}

/*
 * Stop the motor.
 *
 * Sent as soon as a disc has been identified, which is what UniROM does once
 * it no longer needs the drive. Two reasons: there is no point spinning a disc
 * for a dashboard that has finished reading it, and - the practical one - a
 * spinning, seeking drive answers GetStat slowly enough to drop a frame, so
 * the lid poll made the whole top of the screen blink every couple of seconds.
 * A stopped drive answers immediately.
 */
static void cdStopMotor(void) {
	cdBegin();
	cdCommand(CMD_STOP, NULL, 0);
	cdEnd();
}

/* Drive status byte, or 0xff. Used for lid detection; safe now because
 * cdBegin() masks the BIOS handler for the duration. */
static uint8_t cdPollStat(void) {
	cdBegin();

	uint8_t stat = 0xff;

	if (cdCommand(CMD_GETSTAT, NULL, 0) && cdResultLen >= 1)
		stat = cdResult[0];

	cdEnd();
	return stat;
}

/*
 * Pull the game ID out of a SYSTEM.CNF image.
 *
 * The file looks like:
 *     BOOT = cdrom:\SLUS_004.02;1
 *     TCB = 4
 *     ...
 *
 * Take what follows the last '\' or ':' on the BOOT line and stop at ';',
 * which is exactly what the BIOS patch's IDSTRINGBUILDER loop does.
 */
static bool parseBootId(const uint8_t *data, int length,
                        char *out, int outSize) {
	for (int i = 0; i + 4 < length; i++) {
		if (data[i] != 'B' || data[i + 1] != 'O' ||
		    data[i + 2] != 'O' || data[i + 3] != 'T')
			continue;

		int j = i + 4;

		/* Skip spaces and the '=' */
		while (j < length &&
		       (data[j] == ' ' || data[j] == '\t' || data[j] == '='))
			j++;

		/* Walk to the end of the token, remembering the last separator. */
		int start = j;

		while (j < length && data[j] > ' ' && data[j] != ';') {
			if (data[j] == '\\' || data[j] == '/' || data[j] == ':')
				start = j + 1;
			j++;
		}

		int len = j - start;

		if (len <= 0 || len >= outSize)
			return false;

		for (int k = 0; k < len; k++) {
			uint8_t c = data[start + k];

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

int gameIdCanLaunch(void) {
	if (current.noticeTime <= 0)
		return 0;

	return current.state == GAMEID_FOUND || current.state == GAMEID_UNLISTED;
}

void gameIdClearNotice(void) {
	current.noticeTime = 0;
}

/*
 * Read the disc now. Blocking, a few hundred milliseconds at worst, all of it
 * bounded. Called from gameIdPoll() on a lid-close transition.
 */
static void gameIdReadDisc(uint8_t *scratch, int scratchSize) {
	int got = readSystemCnf(scratch, scratchSize);

	if (got == GAMEID_NO_DATA_DISC) {
		/* Audio disc, no disc, or the lid is open. Distinct from a failed
		 * read so the scan loop can stop instead of retrying. */
		current.state = GAMEID_NO_DISC;
		current.id[0] = '\0';
		return;
	}

	if (got <= 0) {
		current.state = GAMEID_NO_DISC;
		current.id[0] = '\0';
		return;
	}

	char bootName[32];

	if (!parseBootId(scratch, got, bootName, sizeof(bootName))) {
		current.state = GAMEID_UNKNOWN;
		current.id[0] = '\0';
		return;
	}

	// Only the game name is interesting on screen - "SLUS-00402" tells the
	// user nothing. A disc that is not in the table falls back to the raw
	// boot name, which at least identifies an import or a homebrew disc.
	const char *name = dbLookup(bootName);
	const char *show = name ? name : bootName;

	int i = 0;

	while (show[i] && i < (int) sizeof(current.id) - 1) {
		current.id[i] = show[i];
		i++;
	}

	current.id[i] = '\0';
	current.state = name ? GAMEID_FOUND : GAMEID_UNLISTED;
}

/*
 * Scan scheduling.
 *
 * The read is unavoidably blocking: the BIOS spins the drive up, seeks and
 * retries, and there is no asynchronous form of open()/read(). A scan
 * therefore costs a visible pause, so scans are deliberately rare and always
 * announced - the card slides in saying "Reading disc..." a frame before the
 * read starts, so the pause reads as loading rather than as a freeze.
 *
 * There is no lid polling. See the note at the top of this file: watching the
 * shell flag means driving the CD-ROM registers behind the BIOS's back, which
 * crashed the dashboard outright. A scan happens once shortly after boot, and
 * thereafter only when something asks for one via gameIdRequestScan().
 */
/*
 * Scan scheduling.
 *
 * WHY THIS RETRIES INSTEAD OF WAITING A FIXED TIME
 * ------------------------------------------------
 * The first scan after boot always worked with a 3 second delay, and the one
 * after a lid close always failed with the same delay - or with 4 seconds.
 * The reason turned out to be that on boot the drive is already spinning:
 * whatever launched the dashboard left it running. After a lid close it is
 * starting from a dead stop and has to spin up AND re-read the table of
 * contents, and how long that takes varies with the disc and the drive.
 *
 * So there is no correct fixed delay. Instead the scan retries: an initial
 * settle, then an attempt every two seconds until the disc reads or the
 * window runs out. A cold drive that needs fifteen seconds is fine.
 *
 * Failures are silent. The notification only ever appears when a name was
 * actually found - reporting "Disc not readable" while the drive is still
 * spinning up was the thing that made this feel broken, and it was never
 * useful information.
 */
#define SCAN_SETTLE_BOOT   180   /* 3s  - drive is usually already spinning */
#define SCAN_SETTLE_LID    300   /* 5s  - cold start after a swap           */
#define SCAN_RETRY_GAP     240   /* 4s  between attempts                    */
/*
 * Five attempts, four seconds apart: the same ~20 second window as before,
 * but half the number of failed reads inside it.
 *
 * This is the only lever pulled for the audio-CD lag. A disc that is going to
 * read succeeds on the first or second attempt once the drive is up, so a
 * good disc never notices; an audio disc, which can never read, now burns
 * five failed attempts instead of ten or twelve.
 */
#define SCAN_MAX_ATTEMPTS  5

static int scanSettle    = -1;   /* frames until the first attempt */
static int scanAttempts  = 0;    /* attempts left, 0 = not scanning */
static int chimeDelay    = -1;

static void gameIdStartScan(int settleFrames) {
	scanSettle   = settleFrames;
	scanAttempts = SCAN_MAX_ATTEMPTS;
}

void gameIdRequestScan(void) {
	/* Manual R1: the user has decided the drive has had long enough, so go
	 * almost immediately - but still retry, since they may be early. */
	gameIdStartScan(30);
}

void gameIdPoll(uint8_t *scratch, int scratchSize) {
	static bool wasOpen   = false;
	static bool primed    = false;
	static int  bootDelay = 60;
	static int  pollDelay = 0;

	if (current.noticeTime > 0)
		current.noticeTime--;

	if (chimeDelay > 0) {
		chimeDelay--;

		if (chimeDelay == 0) {
			gameIdNoticeSound();
			chimeDelay = -1;
		}
	}

	if (scratchSize < GAMEID_SCRATCH_SIZE)
		return;

	/* First scan, shortly after boot. */
	if (bootDelay > 0) {
		bootDelay--;

		if (bootDelay == 0)
			gameIdStartScan(SCAN_SETTLE_BOOT);

		return;
	}

	/* --- a scan is in progress ------------------------------------------ */
	if (scanAttempts > 0) {
		if (scanSettle > 0) {
			scanSettle--;
			return;
		}

		scanAttempts--;
		gameIdReadDisc(scratch, scratchSize);

		if (current.state == GAMEID_NO_DISC) {
			/*
			 * The drive told us there is nothing readable in there - an
			 * audio CD, an empty drive, or an open lid. Retrying cannot
			 * change that answer, so stop now rather than grinding through
			 * the rest of the window. This is what made an audio disc lag
			 * the dashboard for half a minute.
			 */
			scanAttempts  = 0;
			current.state = GAMEID_IDLE;
			current.id[0] = '\0';
			cdStopMotor();
			return;
		}

		if (current.state == GAMEID_FOUND ||
		    current.state == GAMEID_UNLISTED) {
			/* Success: this is the only path that shows anything. */
			scanAttempts       = 0;
			current.noticeTime = GAMEID_NOTICE_FRAMES;
			chimeDelay         = 60;

			/* Nothing more to read - park the drive. */
			cdStopMotor();
			return;
		}

		/* Not ready yet. Say nothing and try again shortly. */
		current.state = GAMEID_IDLE;
		current.id[0] = '\0';

		if (scanAttempts > 0) {
			scanSettle = SCAN_RETRY_GAP;
		} else {
			/*
			 * Out of attempts. Stop the motor and say nothing.
			 *
			 * This is the audio-CD path as much as the no-disc one: an audio
			 * disc has no data track, so every attempt fails and the window
			 * simply expires. Nothing is shown, nothing keeps retrying, and
			 * the drive is left parked rather than spinning.
			 */
			cdStopMotor();
		}

		return;
	}

	/* --- otherwise, watch the lid --------------------------------------- */
	if (pollDelay > 0) {
		pollDelay--;
		return;
	}

	/* A stopped drive answers instantly, so this is cheap - but there is no
	 * reason to ask more than about twice a second. */
	pollDelay = 30;

	uint8_t stat = cdPollStat();

	if (stat == 0xff)
		return;

	bool isOpen = (stat & 0x10) != 0;   /* ShellOpen */

	if (!primed) {
		/* Adopt the first reading without reacting, so booting with the lid
		 * already shut does not look like an insertion. */
		primed  = true;
		wasOpen = isOpen;
		return;
	}

	if (isOpen && !wasOpen) {
		/* Lid opened: forget the old disc and stop showing its name. */
		current.state      = GAMEID_IDLE;
		current.id[0]      = '\0';
		current.noticeTime = 0;
	}

	if (!isOpen && wasOpen)
		gameIdStartScan(SCAN_SETTLE_LID);

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

	/*
	 * Only a successful identification is ever shown.
	 *
	 * The failure states still exist so the scan loop can tell them apart,
	 * but they never reach the screen: a card saying "Disc not readable"
	 * while the drive is still spinning up was actively misleading, and the
	 * retry a couple of seconds later usually succeeded anyway.
	 */
	const char *value = NULL;

	if (current.state == GAMEID_FOUND || current.state == GAMEID_UNLISTED)
		value = current.id;

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

	/*
	 * "<game>  (START) to Launch" - the hint is part of the same card rather
	 * than a second line, so the notification stays one line tall.
	 */
	char line[96];
	int  i = 0;

	while (value[i] && i < (int) sizeof(line) - 24) {
		line[i] = value[i];
		i++;
	}

	line[i] = '\0';

	static const char hint[] = "  " CH_PS1_START_BUTTON " Launch";
	int j = 0;

	while (hint[j] && i < (int) sizeof(line) - 1)
		line[i++] = hint[j++];

	line[i] = '\0';
	value   = line;

	int textW = getStringWidth(value);
	int boxW  = textW + 18;

	if (boxW > 300)
		boxW = 300;

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
