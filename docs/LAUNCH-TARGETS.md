# Why each launch target needs what it needs

Every setting below was established by testing on real hardware. They are fixed
in `src/main/launch_ui.c` rather than exposed as options - the user presses X
and it works. This file exists so the reasoning is not lost.

| Target | Loads at | Overwrites dashboard? | Path | Erase RAM | Why |
|---|---|---|---|---|---|
| `cdloader.exe` (Fast Boot) | `0x801EA300` | no | direct | no | Loads at the top of RAM. Never needed anything special |
| SIO loader | `0x801B0000` | no | direct | no | Bare-metal, one BIOS call in its life (`FlushCache`). Works under every combination; direct disturbs least |
| UniROM 8.0 | `0x801D0000` | no | direct | no | Works under every combination *now*. See the TTY note below |
| 240p Test Suite | `0x80010000` | **yes** | **stage 1** | **yes** | Lands on the dashboard, so the trampoline is mandatory. Declares **no BSS**, so it needs RAM zeroed first |

## The two things that actually mattered

**1. The TTY device — this is what broke UniROM.**

`main.c` used to call `installSerialTTY()`, putting a custom `"tty"` into the
kernel's device table. UniROM's own start-up calls `RemoveDevice("tty")` before
installing its redirect - and `RemoveDevice` invokes the *existing* driver's
deinit handler. Ours wrote to SIO1 and spun forever waiting for TX-ready with
no cable attached. UniROM was never broken; it was hanging inside our driver.

This was proved by accident: an attempt to remove the device at hand-off time
froze the dashboard at exactly the same point, on the same instruction.

The TTY subsystem is now deleted outright. Serial is the standalone SIO
loader's job, so the dashboard gained nothing from it. This also fixes any
other homebrew that replaces the TTY device.

**2. 240p needs RAM erased because it declares no BSS.**

Its PS-EXE header has `bssAddr = 0` and `bssSize = 0`, so nothing - not the
BIOS, not the loader - zeroes its uninitialised data. It gets away with that
from a cold boot, or over serial from the SIO loader, because the RAM it lands
in happens to be clear. After this dashboard has been running, that RAM is full
of our leftovers, and it starts with garbage globals.

Erasing RAM before the jump is what makes it work. It is the only target here
that needs it, and it is also the only one that loads low enough to require the
stage 1 trampoline at all - so it exercises the most machinery of the four.

## The bug that made all of this hard to see

`app_stub.s` is assembled with `.set noreorder`, and the R3000 has **no load
interlock**: the instruction after an `lw` still sees the register's old value.
The copy loop read a word and stored it in the very next instruction, so every
store wrote the *previous* iteration's word and the destination came out
shifted by one.

It hid for a long time because the SIO loader - the only other user of the stub
- receives its payload straight into place and passes a copy count of **zero**.
The loop never ran. 240p is the first target that actually needed a copy.

Fixed in four places: both copy loops, the verify compare, and the fill-list
head pointer. Every `lw` in that file is now followed by a `nop` unless a real
instruction fills the slot. **Anything added to `app_stub.s` must respect
this** - there is no tooling that will catch it.

## Diagnostics still in place

- Stage 1 verifies the copy word for word before continuing. A mismatch paints
  the screen dark olive (`0x1F1F`) and stops, so a corrupt copy can never
  silently launch garbage.
- Progress marks (red/green/yellow/orange through the hand-off) are compiled
  out via `APP_STUB_MARKS` in `app_stub.s`. Set it to `1` to bring them back.
- `planEmbeddedApp()` refuses impossible launches with a reason rather than
  jumping; `showPlanError()` displays it.

## Build-time guard

`make -C tools -f Makefile.tests` starts with a `check-entry-points` step that
verifies every `void runX(...)` declared in `launch_ui.h` has a matching
definition in `launch_ui.c`.

This exists because the menu tables in `xmb_menu.c` are the only things that
reference those functions, so deleting one is not a compile error - it surfaces
minutes later as an `undefined reference` from the MIPS linker, pointing at a
different file. That happened twice during development. The check is plain text
matching, runs in under a second, and CI runs it before the toolchain is even
installed.

## Adding another app later

1. Put the `.exe` in `assets/`.
2. `addBinaryFile("${RELEASE_NAME}" myAppExe assets/myapp.exe)`.
3. Add a `LaunchConfig` in `launch_ui.c` and a `runLaunchScreen()` wrapper.

Start with `{ 0, 0 }` (direct, no erase). If the app loads at `0x80010000` the
planner will select stage 1 automatically. If it starts and then misbehaves -
garbage on screen, wrong colours, immediate crash - check whether its header
declares a BSS; if it does not, try `{ 1, 0 }`.
