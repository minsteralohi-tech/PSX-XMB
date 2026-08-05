/*
 * PSX-iTests - UI sound effects
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Uploads the UI sound effects to SPU RAM. Must be called once after
// initSPU().
void initSound(void);

// Total SPU RAM (bytes, out of SPU_RAM_SIZE) actually occupied after
// initSound() - all 9 SFX samples, the currently-resident BGM track's
// shared slot, and the SPU test tone. For the HUD's SPU RAM meter.
uint32_t getSPURAMUsedBytes(void);

// Plays the "navigation/cursor" sound (menu scrolling - up/down, category
// switching, etc).
void playScrollSound(void);

// Plays the "confirm/select" sound, on its own channel so a rapid
// scroll-then-confirm doesn't cut either sound off early.
void playConfirmSound(void);

// Plays the "back/exit/cancel" sound - distinct from both of the above,
// its own channel and its own sample. Used wherever Circle (or an
// equivalent "back one level") actually backs out of something, as
// opposed to just being an alternate confirm/dismiss button.
void playCancelSound(void);

// Starts the looping background music. Call once at startup, after
// initSound(). The SPU loops it in hardware from then on - no per-frame
// code needed anywhere else.
void playBGM(void);

// Plays the dedicated SPU test tone on an arbitrary channel (0-23). Used
// by the SPU channel test to check each channel individually. Note that
// channels 0-3 are normally used for scroll/confirm/cancel/BGM - testing
// those channels will interrupt whatever they were doing, so the caller
// is responsible for restoring normal playback afterward (see
// initSound()/playBGM()).
/* System notification chime - the disc detection card. Independent of the
 * user's chosen SFX set, but still muted by the SFX toggle. */
void playNotifySound(void);

/*
 * Boot jingle. Borrows the BGM slot (see the note in sound.c), so
 * restoreBGMAfterIntro() must be called once the boot sequence ends.
 */
void playIntroJingle(void);
void restoreBGMAfterIntro(void);

void playTestTone(int channel);

// Toggles background music on/off (mutes/restores its volume - doesn't
// stop the SPU from looping it, just silences it). Returns the new state.
bool toggleBGM(void);
bool isBGMEnabled(void);

// --- Background music track selection -----------------------------------
// The project ships more than one looping BGM. They can't all live in SPU RAM
// at once (each several hundred KB, SPU RAM is 512 KB), so exactly one is
// resident at a time in a shared slot; selecting a track re-uploads it and
// restarts the loop, preserving the current on/off (mute) state.
int         getBGMCount(void);
int         getBGMIndex(void);
const char *getBGMName(int index);

/*
 * Whether a listed track is actually embedded in this build. Tracks can be
 * dropped from CMakeLists.txt to free RAM; they stay in the list so they do
 * not silently vanish, but the picker greys them out and selectBGM() refuses
 * them. Returns 0 for an unavailable or out-of-range index.
 */
int bgmTrackAvailable(int index);
// Switch to BGM track `index` (0..getBGMCount()-1). No-op if already current.
void        selectBGM(int index);

// --- SFX set selection ----------------------------------------------------
// 3 sets (PSP/MGS/PS4), each with its own nav/confirm/cancel sound. All 9
// samples stay resident in SPU RAM at once (they're small), so switching
// sets is instant - no re-upload like selectBGM() needs.
int         getSFXSetCount(void);
int         getSFXSetIndex(void);
const char *getSFXSetName(int index);
void        selectSFXSet(int index);

// Toggles scroll/confirm/cancel UI sound effects on/off. Doesn't affect BGM
// or playTestTone(). Returns the new state.
bool toggleSFX(void);
bool isSFXEnabled(void);

#ifdef __cplusplus
}
#endif
