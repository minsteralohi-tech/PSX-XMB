# Two-stage app launcher — what changed and why

The dashboard no longer contains a serial receiver. Serial loading is now the
standalone SIO loader PS-EXE (`assets/sioloader.exe`), embedded as a blob and
started through a generic two-stage handoff that is reusable for any other
standalone program — UniROM, a 240p test suite build, whatever comes next.

## Removed

| File | Why |
|---|---|
| `src/main/sio_loader.c` (535 lines) | The whole dashboard-resident NoPS receiver. Superseded by the standalone loader |
| `src/main/sio_loader.h` | ditto |
| `src/main/sio_staged_stub.s` | Its trampoline, pinned at `0x8000c800` |
| `src/main/sio_miniloader.s` | Already dead — was not in the CMake source list |
| `launchStagedPSEXE()` in `handoff.c`/`.h` | The staged-receive handoff it existed to serve is gone |

`quiesceForHandoff()`, `launchPSEXEImage()` and `jumpToLoadedEXE()` are
untouched. Fast Boot and Tools → UniROM 8.0 still use the exact path they use
today; this change does not go near them.

## Added

| File | Role |
|---|---|
| `src/main/app_launch.h` / `.c` | Plans and executes a two-stage handoff for any embedded PS-EXE |
| `src/main/app_stub.s` | Stage 1: ~230 bytes, position independent, no absolute addresses |
| `src/main/sio_launch.h` / `.c` | Settings → SIO Loader: shows the plan, confirms, launches |
| `assets/sioloader.exe` | The proven standalone loader, 8192 bytes |
| `tools/test_app_launch.c`, `tools/Makefile.tests` | Host tests for the planner |

`CMakeLists.txt` swaps the source list entries and adds
`addBinaryFile(... sioLoaderExe assets/sioloader.exe)`. `xmb_menu.c` changes by
exactly one line — its `#include` — so the `settingsItems[]` table and the
`runSIOLoader` entry name stay as they were.

## Why two stages

The dashboard is huge. Its PS-EXE payload is about 1.59 MB, so it occupies
roughly `0x80010000`–`0x8019c800` before `.bss`, and `.bss` pushes `_imageEnd`
higher still. The PCSX-Redux session log confirms the runtime shape:

```
pc=800118ec sp=8019e0c8 gp=801a41d0
```

The stack is a static 8 KB buffer inside the image (`crt0.s`), not at the top
of RAM, and the heap grows from `_bssEnd` toward `0x80200000`.

Almost anything worth launching wants RAM the dashboard is sitting in. The
old single-stage `launchPSEXEImage()` copies the payload from C code that is
executing out of the region being overwritten — it works today only because
the embedded blobs happen to sit below every load address in use, and it
offers no way to clear the dashboard out of RAM at all, because the code doing
the clearing would be the first casualty.

So:

```
stage 0   dashboard   pick an arena, validate, quiesce, install stage 1
stage 1   arena       copy payload -> zero-fill -> memfill BSS ->
                      FlushCache -> set $gp/$sp -> jump
stage 2   target      the launched program
```

Nothing returns to C once stage 1 starts.

## Stage 1 is used only when it is actually needed

**This is the fix for the black screen / frozen warning screen.**

Copying the payload straight from C and jumping — `handoff.c`'s
`launchPSEXEImage()` — is safe whenever the destination and the memfill are
clear of the code doing the copying and of its stack. That is true for the
standalone SIO loader (`0x801b0000`) and for UniROM (`0x801d0000`), and it is
the **only handoff on this console with a track record of working**: Fast Boot
and Tools → UniROM 8.0 both use it.

The trampoline path is the one that has never worked here — the original
dashboard SIO loader transferred perfectly and then died in
`launchStagedPSEXE()`, whose stub ran from `0x8000c800`.

The first version of this launcher put *everything* through a trampoline,
including the SIO loader, which did not need one. Worse, its arena rule
required the arena to sit above `_imageEnd`. With this dashboard's `.bss`,
`_imageEnd` is high enough that every main-RAM candidate was rejected and the
choice fell through to `0x8000c000` — BIOS kernel scratch, i.e. the exact
configuration that has never worked.

So the planner now decides:

| Case | Path |
|---|---|
| destination and memfill clear the dashboard's `.text` and stack | **direct** — no trampoline, no arena |
| they do not, or RAM is being erased | **stage 1** |

For the SIO loader this selects **direct**, and the confirmation screen says
so. "Live" means `.text` plus the current stack — not `_imageEnd`. Once
`quiesceForHandoff()` has run, `.bss`, the heap and everything above the stack
is dead, because nothing reads it again.

## Picking the arena

There is no single address that is free for every target, which is what made
the old fixed `0x8000c800` fragile:

| Target | Occupies |
|---|---|
| standalone SIO loader | `0x801b0000`–`0x801c0000` |
| UniROM 8.0.K | `0x801d0000`–`0x801f1000` |
| `cdloader.exe` | `0x801ea300`–`0x80200000` |
| ordinary homebrew | `0x80010000` upward |

So the arena is chosen at run time from a candidate list — `0x801ff000`,
`0x801e0000`, `0x801c8000`, then BIOS kernel scratch `0x8000c000` as a last
resort — taking the first that provably clears the destination, the source
blob and the target's BSS. Candidates in main RAM must also sit at or above
the dashboard's own `_imageEnd`, so installing stage 1 cannot corrupt a global
the dashboard is still reading between the install and the jump.

Kernel scratch is last on purpose: nothing documents it as free, and the
kernel's event/thread control blocks — plus a resident cheat cartridge's
hooks — live in that 64 KB. It exists only so a target wanting all of
`0x801c8000`–`0x80200000` still has somewhere to put stage 1.

For the SIO loader the planner picks `0x801ff000`, and the confirmation screen
shows it before anything is committed.

## Erasing the dashboard

`planEmbeddedApp(exe, eraseRam, &plan)` takes a flag. When set, stage 1 zeroes
all of main RAM except the payload it just copied, the arena, and everything
below `0x80010000`. The fill list is built in C as explicit ranges, with the
arena punched out — which can split one range in two — so stage 1 itself can
never be erased by its own fill.

`SIO_LOADER_ERASE_RAM` in `sio_launch.c` is **0**. The standalone loader
validates and stages everything itself and does not assume a cold-boot RAM
state; the BIOS does not zero RAM on `LoadExec` either. Set it to 1 for a
target that genuinely needs clean RAM — it is safe either way.

## Order of operations in stage 1

1. Move the payload, direction-aware. The blob lives in `.rodata` and the
   destination is frequently *below* it, so both copy directions are used.
2. Walk the zero-fill list. This is where the dashboard is erased and where
   the target's PS-EXE memfill happens.
3. Switch `$sp` to the target's stack **before** the BIOS call — the
   dashboard's stack is inside the image and step 2 may just have zeroed it.
4. BIOS `A(44h)` FlushCache.
5. Set `$gp`, jump to the entry point.

## Tests

`make -C tools -f Makefile.tests` builds `src/main/app_launch.c` with the host
compiler and runs 64 checks: arena selection for the SIO loader, UniROM,
`cdloader.exe` and ordinary homebrew; the arena never landing on the payload,
the source blob, the target's BSS or the dashboard's image; fill ranges never
covering the payload, the arena or BIOS RAM; and rejection of bad magic, zero
size, kernel-RAM destinations, ranges that wrap past 2³², unaligned addresses,
out-of-image entry points and bad stack pointers.

The CI workflow runs this before the toolchain and build steps, so a planner
regression fails in seconds rather than after a full build.

Note one deliberate behaviour the tests pin down: the SIO loader's own `.bss`
starts at `0x801b1770`, *inside* its 0x1800-byte padded body, so the memfill
zeroes the tail of the copied payload. That is correct and is exactly what the
BIOS does after a `LoadExec`.

## Adding another launchable app later

1. Drop the `.exe` in `assets/`.
2. `addBinaryFile("${RELEASE_NAME}" myAppExe assets/myapp.exe)` in
   `CMakeLists.txt`.
3. `extern const uint8_t myAppExe[];` and call
   `launchEmbeddedApp(myAppExe, 0)`, or `planEmbeddedApp()` first if you want
   to show the plan and confirm.

If the planner refuses, it says why rather than jumping — `appPlanResultText()`
gives the reason, and the SIO Loader screen already displays it.

## Also fixed: a register bug in stage 1

The stub kept the parameter-block pointer in `$s7` across the BIOS
`FlushCache` call and reloaded the entry point through it afterwards. That
only works if the BIOS honours the o32 ABI for every callee-saved register — a
bet with no upside. It now preloads the PC, GP and SP into `$s0`/`$s1`/`$s2`
before the call and touches no memory after it, exactly like the trampoline in
the standalone SIO loader that is already proven on this console.

## Testing a launch on hardware without rebuilding

Both Settings -> SIO Loader and Tools -> UniROM 8.0 now show the same
confirmation screen, driven by `planEmbeddedApp()`:

```
Target   801D0000 - 801F1000  (135168 bytes)
Arena    801FF000      Entry 801D0000
Dash end 801C4A70      Live  801A0FF0
Path     Stage 1 trampoline
Erase RAM  YES - wipe the dashboard first
```

**Triangle toggles the RAM erase and re-plans**, which also switches `Path`
between the trampoline and the direct copy:

| Erase RAM | Path | What it exercises |
|---|---|---|
| no | direct | The handoff Fast Boot and UniROM have always used |
| yes | stage 1 | Trampoline, RAM wipe, memfill |

Defaults: SIO loader **no** (proven working, do not disturb), UniROM **yes**.

### About `bios_reinit.c`

There is a `reinitializeBIOSForHandoff()` in `src/main/bios_reinit.c` that
would rebuild the retail kernel state before a handoff - the same warm-boot
reconstruction `cdloader.exe` performs. It is a plausible fix for a
BIOS-dependent target such as UniROM, which installs its own kernel exception
handler and TTY redirect.

**It is not built and not called.** The file is not in `CMakeLists.txt`, and it
would not link if it were: it needs ten BIOS call wrappers
(`biosEnterCriticalSection`, `biosSetConf`, `biosInstallDevices`,
`biosSetMemSize`, ...) that `bios_calls.s` never defined - that file only
provides `biosFlushCache`, `biosOpen`, `biosClose`, `biosAddDevice` and
`biosRemoveDevice`. That is almost certainly why `handoff.h` records the
kernel-rebuild experiment as having been abandoned.

Finishing it means writing those wrappers against the correct A0/B0/C0 table
indices, where a wrong index calls an arbitrary kernel routine. Worth doing
only once there is evidence that stale kernel state is the actual problem -
see the UniROM note below.

## Three hand-off styles

| Path | When | What performs the launch |
|---|---|---|
| Direct | destination and memfill clear the dashboard's `.text` and stack | `jumpToLoadedEXE()` - copy, flush, set `$gp`/`$sp`, jump |
| BIOS `Exec()` | asked for, and the direct path applies | BIOS A0(43h) - the kernel sets `$gp`, builds the stack, fills BSS and calls the entry |
| Stage 1 | payload lands on the copier, or RAM is being erased | position-independent trampoline in the arena |

### Why `Exec()` exists

UniROM starts correctly when the BIOS boots it and black-screens when the
registers are set by hand - including over serial from the standalone SIO
loader, which rules out the dashboard entirely. It installs its own kernel
exception handler and TTY redirect and leans on BIOS services from its first
instruction; the 240p suite and the SIO loader are bare-metal and do not care.

A PS-EXE header from offset `0x10` onwards *is* the BIOS `EXEC` structure, so
the whole hand-off is: copy the payload, `FlushCache`, `Exec(header + 0x10)`.
The kernel then does exactly what it does when booting from disc.

The other half of it is interrupt state. `quiesceForHandoff()` ends by writing
0 to COP0 SR and masking every interrupt source - correct for a bare-metal
target that installs its own handlers, fatal for one that waits on a kernel
event that can then never fire. `quiesceForBIOSExec()` does everything else
(SPU, DMA CHCR, GPU reset, COP0 breakpoints) but leaves the interrupt mask and
SR exactly as they are.

`Exec()` cannot be combined with erasing RAM or with stage 1: it runs kernel
code and returns to the caller on failure, so the dashboard has to still be
there. `planUseBiosExec()` refuses the combination rather than producing a plan
that erases its own escape route.

**Square** toggles it on the confirmation screen. UniROM defaults to `Exec()`
on and erase off; the SIO loader keeps the direct jump that already works.

## Asset budget

The dashboard is ~80% of a 2 MB console, so anything embedded competes with
what can be launched. Measured contents of the 1,624,064-byte payload:

| Item | Bytes |
|---|---:|
| BGM x2 (ps3xmb, ps4xmb) | 598,432 |
| **240p.exe** | **395,264** |
| code + data | ~174,640 |
| unirom_bin.exe | 137,216 |
| textures | 128,992 |
| SFX x9 | 123,504 |
| cdloader.exe | 26,624 |
| sioloader.exe | 8,192 |
| **total** | **1,596,976** |

That ends at `0x80195E30`, leaving 424 KB for `.bss` and the heap.

Fitting the 240p Test Suite needed two things given up:

| Given up | Freed |
|---|---:|
| `sanctuary.vag` dropped from the BGM list | 371,760 |
| lava/earth/moon textures 8bpp -> 4bpp | 50,592 |

`assets/sanctuary.vag` is still in the repo. To bring the track back, uncomment
its `addBinaryFile()` line in `CMakeLists.txt` and restore `bgm3Sound` in
`sound.c`'s `bgmTable[]`/`bgmNames[]` - `BGM_COUNT`, the SPU slot size and the
Music settings list all derive from those two arrays. Something else has to go
first, though: there is no longer room for both it and the 240p suite.

The three roaming planet textures (lava, earth, moon) are 4bpp, saving 50,592
bytes against 8bpp. Their PNGs were requantized to 16 colours with
Floyd-Steinberg dithering, because `tools/convertImage.py` rejects images over
the colour limit instead of quantizing them - so re-exporting one of these at
more than 16 colours will fail the build. The sun stays 8bpp: it is drawn far
larger and banding shows.

Pure black is deliberately absent from those palettes. The GPU treats a
`0x0000` texel as transparent, and none of these textures contained black
before, so introducing it would punch holes in the planets.

Dead files removed (they were in `assets/` but no `addBinaryFile` line ever
referenced them, so they cost disc space only, never RAM): `bgm.vag`,
`newbgm.vag`, `bgm3.vag`, `cdloaderold.exe`, `cdlogo.tmd`, `pickup.vag`,
`dpad.png`, `n00brom.rom`, plus the uncompiled `n00brom_launch.c/.h`.

## The 240p Test Suite and the arena scratch stack

240p loads at `0x80010000` - straight over this dashboard - so it always takes
the stage 1 path; there is no direct-jump or `Exec()` option for it.

It also asks for `$sp = 0x801ffff0`, which is **inside** the arena at
`0x801ff000`. That is fine in itself: stage 1 has jumped before the target
pushes anything. But stage 1 used to switch to the target's stack *before*
calling BIOS `FlushCache`, which would have had the BIOS pushing into the code
it was executing. FlushCache is unlikely to use the ~4 KB needed to actually
reach it, but "unlikely" is not a basis for a hand-off.

Stage 1 now uses its own scratch stack at the top of the arena
(`APP_ARENA_STACK_OFF`, ~1.7 KB clear of the fill list) for the BIOS call, and
only switches to the target's stack in the last three instructions before the
jump. `tools/test_app_launch.c` pins this down so the reason it exists is not
lost.

## Diagnosing a stage 1 failure

Stage 1 paints the whole screen a flat colour at each phase, so a failure says
which step it reached instead of showing a black screen:

| Colour left on screen | Reached | So the failure is in |
|---|---|---|
| nothing / black | stage 1 never entered | the install or the jump in `runStagedLaunch()` |
| red | entered | the payload copy |
| magenta | copy verify failed | the payload copy itself is corrupt |
| green | payload copied **and verified word for word** | the zero-fills |
| yellow | fills done | the scratch-stack switch or BIOS FlushCache |
| orange | FlushCache returned | the target itself, or `$gp`/`$sp` |
| the target's own output | it ran | - |

Set `APP_STUB_MARKS` to 0 in `app_stub.s` to compile them out.

### What the marks found

The 240p suite reached **orange** - stage 1 ran to completion, copied the
payload, flushed the cache and jumped. The fault was never in the trampoline.

It was the TTY device. `tty_serial.c` installs a custom "tty" into the kernel's
device table with `biosAddDevice(&ttyDCB)`, and `ttyDCB` plus every handler it
points at lives inside this dashboard's image at ~`0x8001xxxx`. The 240p suite
loads at `0x80010000` and writes straight over them, leaving the kernel's
device table pointing into the middle of the newly loaded program. The next
BIOS call touching "tty" jumps into it.

That also explains why nothing else ever hit this: cdloader (`0x801EA300`), the
standalone SIO loader (`0x801B0000`) and UniROM (`0x801D0000`) all load *above*
the dashboard, so the dangling pointers still happen to point at intact code.
It looked like a trampoline bug purely because 240p was the first target to
load low.

**Removing the device at hand-off time was the wrong fix and made things
worse.** `RemoveDevice` invokes the driver's own deinit handler, which writes
to SIO1 and spins forever waiting for TX-ready when no cable is attached - so
the hand-off froze the moment X was pressed, before the SPU had even been
silenced (the music kept playing, which is what identified it).

The dashboard now does not install a TTY device at all. `src/main/tty_serial.c`
and `.h` are deleted and `installSerialTTY()` is gone from `main.c`. Serial is
the standalone SIO loader's job now, so the redirect bought nothing, and
leaving the kernel's own TTY untouched means there is nothing dangling to clean
up and no BIOS call to block on. `LOG()` compiles to nothing in release builds,
so nothing else depended on it.

Worth keeping in view: the direct copy-and-jump has worked on three targets
(cdloader, the standalone SIO loader, UniROM-over-serial) and stage 1 has now
failed on three (the original in-dashboard SIO loader, and the 240p suite both
with and without the RAM erase). That asymmetry is the reason these marks
exist - every one of those failures looked identical from the outside.

## The 240p failure was a corrupt copy

The word-for-word verify came back **magenta** - the destination did not match
the source. That rules out the whole class of theories about inherited machine
state, BIOS hooks and register contracts: the program was never intact in RAM
to begin with.

Source and destination were checked and provably do not overlap (`.text` then
`.rodata`, so the blob sits at ~`0x80136270`-`0x80196270` while the destination
is `0x80010000`-`0x80070000`), and both the copy loop and the verify loop are
correct. That leaves cache coherency.

The copy writes over precisely the region the dashboard was executing from
moments earlier, so those addresses are exactly the ones still held in the 4 KB
instruction cache, and the R3000 also has a write buffer between the CPU and
RAM. Stage 1 now performs the copy, the verify and the zero-fills through
**KSEG1** (uncached) by setting bit 29 on the pointers, so every load and store
goes straight to RAM. KSEG0 and KSEG1 address the same physical memory, so the
bytes land in exactly the same place; a one-shot bulk copy at hand-off time
gains nothing from the cache.

## Why ps1-packer --tload does not help here

`--tload` builds an image that loads at a high address and copies itself down
to its real one. On this console that cannot work for the 240p suite, and the
arithmetic is decidable rather than a matter of opinion:

- the dashboard payload ends at `0x80195E30`, with `.bss`, the heap and the
  stack above it (`$sp` is around `0x8019e0c8`);
- a 393,216-byte relocated image would have to land near `0x80196000`, i.e.
  directly on top of that stack. The dashboard would destroy the stack it was
  standing on part-way through the copy.

Doing the first copy from stage 1 avoids that - but then stage 1 copies the
image high and the packed stub copies it down again, and **that second copy is
the same operation stage 1 already performs**. It is the identical mechanism
with two extra steps and one more thing to go wrong, so it cannot fix a fault
in that mechanism.

## Not yet verified

The planner logic is tested on the host. Stage 1 itself, the erase path and
the launch have **not** been run on hardware or in an emulator here. The
assembly is a direct extension of the trampoline in the standalone loader that
is already proven on your console — the copy, cache flush and register handoff
are unchanged; the fill-list walk is new.

Worth testing in this order:

1. Settings → SIO Loader → confirm the plan screen shows
   `Loader 801B0000 - 801B1800`, `Stage 1 801FF000`.
2. Press X; the loader's blue waiting band should appear.
3. Send an EXE and confirm it launches, as it already does standalone.
4. Only then try flipping `SIO_LOADER_ERASE_RAM` to 1, which exercises the
   fill path.
