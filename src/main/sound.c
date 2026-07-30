/*
 * PSX-iTests - UI sound effects
 *
 * Three distinct one-shot .VAG samples (standard PS1 SPU-ADPCM), each on its
 * own dedicated channel: navigation/scroll, confirm/select, and back/cancel.
 * Each is genuinely a different sample now - this used to be a single
 * shared sample played on two channels depending on trigger, but with a
 * proper cancel sound added there's no reason to keep them tied together.
 *
 * Built entirely on top of this project's own common/spu.c driver and
 * ps1/registers.h definitions, no external SDK involved.
 */

#include <stdbool.h>
#include <stdint.h>
#include "common/spu.h"
#include "main/sound.h"
#include "ps1/registers.h"

// The nav/confirm/cancel .vag data for each of the 3 selectable SFX sets,
// embedded via addBinaryFile() in CMakeLists.txt. All kept at full 44.1 kHz
// - none of these are more than ~1s long, so none of them meaningfully
// compete with the BGM tracks for RAM budget.
extern const uint8_t sfxPspNavSound[],     sfxPspConfirmSound[],     sfxPspCancelSound[];
extern const uint8_t sfxMgsNavSound[],     sfxMgsConfirmSound[],     sfxMgsCancelSound[];
extern const uint8_t sfxPs4NavSound[],     sfxPs4ConfirmSound[],     sfxPs4CancelSound[];

typedef struct {
	const uint8_t *nav, *confirm, *cancel;
} SFXSet;

// All 3 sets stay resident in SPU RAM simultaneously (see initSound()) -
// unlike BGM tracks, these are small enough that there's no need for the
// shared-slot/re-upload dance. selectSFXSet() just changes which set's
// offsets get used.
static const SFXSet sfxSets[] = {
	{ sfxPspNavSound, sfxPspConfirmSound, sfxPspCancelSound },
	{ sfxMgsNavSound, sfxMgsConfirmSound, sfxMgsCancelSound },
	{ sfxPs4NavSound, sfxPs4ConfirmSound, sfxPs4CancelSound },
};
static const char *const sfxSetNames[] = { "PSP", "MGS", "PS4" };
#define SFX_SET_COUNT ((int)(sizeof(sfxSets) / sizeof(sfxSets[0])))

static int currentSFXSet = 0;

// The spu.vag data embedded via addBinaryFile() in CMakeLists.txt - a
// dedicated tone used only by the SPU channel test, distinct from the UI
// sound effects.
extern const uint8_t spuTestSound[];

// The three BGM tracks (ps3xmb.vag / ps4xmb.vag / sanctuary.vag), embedded
// via addBinaryFile(). Downsampled to 11025 Hz rather than 44.1 kHz - PS-ADPCM
// size scales directly with sample rate, and at full quality these three
// tracks combined (~155 s) would need close to 2 MB on their own, nowhere
// near fitting alongside everything else this project embeds. 11025 Hz was
// chosen to leave real headroom (~445 KB) for 1-2 more tracks later, not
// just to scrape by on these three - see tools/wav2vag.py's --rate.
extern const uint8_t bgm1Sound[];
extern const uint8_t bgm2Sound[];
extern const uint8_t bgm3Sound[];

// All selectable BGM tracks. They share a single SPU RAM slot (sized for the
// largest one in initSound()); selectBGM() swaps which one is loaded. To add
// more later, embed the .vag via addBinaryFile() and add it here.
static const uint8_t *const bgmTable[] = { bgm1Sound, bgm2Sound, bgm3Sound };
static const char *const bgmNames[]    = { "PS3 XMB", "PS4 XMB", "Sanctuary" };
#define BGM_COUNT ((int)(sizeof(bgmTable) / sizeof(bgmTable[0])))

static int            currentBGM     = 0;
static const uint8_t *currentBGMData = 0;   // set in initSound()

#define SCROLL_CHANNEL  0
#define CONFIRM_CHANNEL 1
#define CANCEL_CHANNEL  2
#define BGM_CHANNEL     3

// Background music plays at half the volume of the UI sound effects.
#define BGM_VOLUME (SPU_MAX_VOLUME / 2)

// Standard 48-byte .VAG header. All multi-byte fields are big-endian.
typedef struct {
	char     id[4];        // "VAGp"
	uint32_t version;
	uint32_t reserved;
	uint32_t dataSize;
	uint32_t sampleRate;
	uint8_t  reserved2[12];
	char     name[16];
} VAGHeader;

static uint32_t swapEndian(uint32_t value) {
	return ((value & 0x000000ff) << 24)
	     | ((value & 0x0000ff00) <<  8)
	     | ((value & 0x00ff0000) >>  8)
	     | ((value & 0xff000000) >> 24);
}

// [set][0=nav,1=confirm,2=cancel] - all 9 uploaded once in initSound(),
// see the SFXSet comment above for why this doesn't need BGM's
// shared-slot/re-upload approach.
static uint32_t sfxOffsets[SFX_SET_COUNT][3];
static uint32_t bgmSoundOffset     = 0;
static uint32_t spuTestSoundOffset = 0;
static uint32_t spuRAMUsedBytes    = 0;   // set in initSound(), see getSPURAMUsedBytes()

static bool bgmEnabled = true;
static bool sfxEnabled = true;

// Uploads one .VAG to SPU RAM at `offset`, returning the aligned size (SPU
// DMA transfers are done in 64-byte blocks) so the caller can place the
// next asset right after it.
static uint32_t uploadVAG(const uint8_t *data, uint32_t offset) {
	const VAGHeader *header = (const VAGHeader *) data;
	uint32_t dataSize    = swapEndian(header->dataSize);
	uint32_t alignedSize = (dataSize + 63) & ~((uint32_t) 63);

	sendSPURAMData(data + sizeof(VAGHeader), offset, alignedSize);
	waitForSPUDMADone();

	return alignedSize;
}

void initSound(void) {
	uint32_t offset = SPU_RAM_ALLOC_OFFSET;

	for (int set = 0; set < SFX_SET_COUNT; set++) {
		sfxOffsets[set][0] = offset;
		offset += uploadVAG(sfxSets[set].nav, offset);

		sfxOffsets[set][1] = offset;
		offset += uploadVAG(sfxSets[set].confirm, offset);

		sfxOffsets[set][2] = offset;
		offset += uploadVAG(sfxSets[set].cancel, offset);
	}

	// Reserve one shared BGM slot, sized for the LARGEST track, so
	// selecting a different (possibly bigger) BGM later can never overrun
	// into the SPU test tone that follows it.
	uint32_t bgmSlotSize = 0;
	for (int i = 0; i < BGM_COUNT; i++) {
		const VAGHeader *h = (const VAGHeader *) bgmTable[i];
		uint32_t aligned   = (swapEndian(h->dataSize) + 63) & ~((uint32_t) 63);
		if (aligned > bgmSlotSize)
			bgmSlotSize = aligned;
	}

	bgmSoundOffset = offset;

	// Load the currently-selected BGM into the slot (default: track 0).
	currentBGMData = bgmTable[currentBGM];
	uploadVAG(currentBGMData, bgmSoundOffset);

	// Place the SPU test tone right after the reserved BGM slot.
	spuTestSoundOffset = bgmSoundOffset + bgmSlotSize;
	spuRAMUsedBytes     = spuTestSoundOffset + uploadVAG(spuTestSound, spuTestSoundOffset);
}

uint32_t getSPURAMUsedBytes(void) {
	return spuRAMUsedBytes;
}

static void playSample(
	const uint8_t *soundData,
	uint32_t      soundOffset,
	int           channel
) {
	const VAGHeader *header = (const VAGHeader *) soundData;

	uint32_t sampleRate = swapEndian(header->sampleRate);
	uint32_t pitch       = (sampleRate << 12) / 44100;

	// Stop the channel first in case it's still playing from a previous
	// trigger, then reconfigure and key it back on.
	if (channel < 16)
		SPU_KOFF0 = 1 << channel;
	else
		SPU_KOFF1 = 1 << (channel - 16);

	SPU_CH_VOLL (channel) = SPU_MAX_VOLUME;
	SPU_CH_VOLR (channel) = SPU_MAX_VOLUME;
	SPU_CH_PITCH(channel) = pitch;
	SPU_CH_SSA  (channel) = soundOffset / SPU_RAM_ADDR_UNIT;
	// Instant attack, slowest decay, max sustain level: holds at full
	// volume until the sample's own end-of-data flag stops it.
	SPU_CH_ADSR1(channel) = 0x00ff;
	SPU_CH_ADSR2(channel) = 0x0000;

	if (channel < 16)
		SPU_KON0 = 1 << channel;
	else
		SPU_KON1 = 1 << (channel - 16);
}

void playScrollSound(void) {
	if (sfxEnabled)
		playSample(sfxSets[currentSFXSet].nav, sfxOffsets[currentSFXSet][0], SCROLL_CHANNEL);
}

void playConfirmSound(void) {
	if (sfxEnabled)
		playSample(sfxSets[currentSFXSet].confirm, sfxOffsets[currentSFXSet][1], CONFIRM_CHANNEL);
}

void playCancelSound(void) {
	if (sfxEnabled)
		playSample(sfxSets[currentSFXSet].cancel, sfxOffsets[currentSFXSet][2], CANCEL_CHANNEL);
}

void playTestTone(int channel) {
	playSample(spuTestSound, spuTestSoundOffset, channel);
}

void playBGM(void) {
	if (!currentBGMData)
		currentBGMData = bgmTable[currentBGM];
	const VAGHeader *header = (const VAGHeader *) currentBGMData;

	uint32_t sampleRate = swapEndian(header->sampleRate);
	uint32_t pitch       = (sampleRate << 12) / 44100;

	SPU_KOFF0 = 1 << BGM_CHANNEL;

	SPU_CH_VOLL (BGM_CHANNEL) = BGM_VOLUME;
	SPU_CH_VOLR (BGM_CHANNEL) = BGM_VOLUME;
	SPU_CH_PITCH(BGM_CHANNEL) = pitch;
	SPU_CH_SSA  (BGM_CHANNEL) = bgmSoundOffset / SPU_RAM_ADDR_UNIT;
	// Loop start address: where the SPU jumps back to once it reaches the
	// End+Repeat flag encoded on the sample's final ADPCM block (see
	// wav2vag.py's --loop option). Pointing it at the sample's own start
	// makes the whole clip loop seamlessly, entirely in hardware - no
	// per-frame code needed anywhere else in the project.
	SPU_CH_LSAX (BGM_CHANNEL) = bgmSoundOffset / SPU_RAM_ADDR_UNIT;
	SPU_CH_ADSR1(BGM_CHANNEL) = 0x00ff;
	SPU_CH_ADSR2(BGM_CHANNEL) = 0x0000;

	SPU_KON0 = 1 << BGM_CHANNEL;

	// playBGM() always makes BGM audible - keep the tracked enabled flag
	// in sync, otherwise a later toggleBGM() call can desync from the
	// real audio state (this was a real bug: a pause/restore cycle via
	// toggleBGM()+playBGM() left bgmEnabled stuck at false even though
	// BGM was audibly playing, so a second such cycle silently skipped
	// pausing it at all).
	bgmEnabled = true;
}

bool toggleBGM(void) {
	bgmEnabled = !bgmEnabled;

	// Mute/restore volume directly rather than stopping/restarting
	// playback - the SPU keeps looping it in hardware regardless, this
	// just silences the channel without touching SPU RAM or the loop
	// state at all.
	uint16_t volume = bgmEnabled ? BGM_VOLUME : 0;
	SPU_CH_VOLL(BGM_CHANNEL) = volume;
	SPU_CH_VOLR(BGM_CHANNEL) = volume;

	return bgmEnabled;
}

bool isBGMEnabled(void) {
	return bgmEnabled;
}

int getBGMCount(void) { return BGM_COUNT; }
int getBGMIndex(void) { return currentBGM; }

const char *getBGMName(int index) {
	if (index < 0 || index >= BGM_COUNT)
		return "";
	return bgmNames[index];
}

void selectBGM(int index) {
	if (index < 0 || index >= BGM_COUNT || index == currentBGM)
		return;

	currentBGM     = index;
	currentBGMData = bgmTable[index];

	// Full re-init (re-uploads all 9 SFX samples, the newly-selected BGM
	// track, and the SPU test tone), not just a re-upload of the BGM slot
	// alone. This is deliberately the exact same sequence boot
	// (main.c) and the CD player's own exit path already use - both
	// reliably start the new/restored track, whereas a leaner "just stop
	// the channel, re-upload the BGM slot, key back on" version of this
	// function did not (reported: selecting a new track from the Music
	// flyout didn't start it, but then visiting and exiting the CD player
	// - which calls this exact initSound()+playBGM() sequence - did). The
	// extra SFX re-uploads cost a few milliseconds of SPU DMA, not
	// something worth trading reliability for on a manual, infrequent
	// track-selection action.
	initSound();

	// Restart the loop, preserving the user's current on/off choice:
	// playBGM() always keys it audibly (and sets bgmEnabled = true), so if
	// the user had music muted, mute it straight back.
	bool wasEnabled = bgmEnabled;
	playBGM();
	if (!wasEnabled)
		toggleBGM();
}

int getSFXSetCount(void) { return SFX_SET_COUNT; }
int getSFXSetIndex(void) { return currentSFXSet; }

const char *getSFXSetName(int index) {
	if (index < 0 || index >= SFX_SET_COUNT)
		return "";
	return sfxSetNames[index];
}

void selectSFXSet(int index) {
	if (index < 0 || index >= SFX_SET_COUNT)
		return;

	currentSFXSet = index;
}

bool toggleSFX(void) {
	sfxEnabled = !sfxEnabled;
	return sfxEnabled;
}

bool isSFXEnabled(void) {
	return sfxEnabled;
}
