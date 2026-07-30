
# PSX-iTests

A basic CPU/GPU/SPU functionality test tool for the original Sony
PlayStation - a simple, cool-looking way to sanity-check that a console's
core hardware is actually working, rather than an emulator-accuracy
validation suite. If you're looking for the latter, check out
[JaCzekanski/ps1-tests](https://github.com/JaCzekanski/ps1-tests) instead.

Currently includes:

- **GPU: Color bar test pattern** - a classic vertical color bar pattern plus
  a gradient strip, for a quick visual check of color output and screen
  geometry.
- **CPU: Benchmark** - runs a fixed workload and reports an iterations/second
  score, timed against real vblank periods (NTSC/PAL aware) rather than the
  Timer registers directly.
- **SPU: Channel test** - steps through all 24 SPU channels individually,
  playing a short test tone on whichever one is selected, to confirm each
  channel actually outputs sound.
- Background music and UI sound effects, to prove the SPU can do more than
  just beep.

A GPU 3D test (spinning cube, using the GTE) is planned but not yet wired in.

## Credits and license

This project is built on top of
[spicyjpeg's ps1-bare-metal](https://github.com/spicyjpeg/ps1-bare-metal)
support library and build system, and reuses the menu/UI framework from
[spicyjpeg's ps1-ram-tester](https://github.com/spicyjpeg/ps1-ram-tester)
almost entirely as-is. The GPU, SPU, controller and reboot drivers
(`src/common/`), the standard library shim (`src/libc/`), the hardware
register definitions (`src/ps1/`), and the font/renderer/UI/modal framework
(`src/main/font.c`, `renderer.c`, `ui.c`, `modals.c`) are all his work,
carried over with minimal or no changes.

New for this project: the main menu (`src/main/mainmenu.c`), the three test
screens (`gpu_colorbars.c`, `cpu_bench.c`, `spu_channel_test.c`), and the
sound module (`sound.c`) that drives the UI sounds and looping BGM.

Everything in this repository is licensed under the MIT license (or the
functionally equivalent ISC license), same as the upstream projects it's
built on. See `LICENSE` for the full text. The only "hard" requirements are
attribution and preserving the license notice; you may otherwise freely use
any of the code for both non-commercial and commercial purposes.

## Building

This repository follows the same overall structure as ps1-bare-metal, so you
may refer to
[its build instructions](https://github.com/spicyjpeg/ps1-bare-metal#building-the-examples).

A working GitHub Actions workflow is included (`.github/workflows/build.yml`)
that builds its own MIPS toolchain from scratch (cached after the first run),
downloads the latest [mkpsxiso](https://github.com/Lameguy64/mkpsxiso) Linux
release automatically, and uploads the compiled CD image / executable as a
downloadable artifact - no local toolchain setup required if you'd rather
build in CI.

The build produces a `.bin`/`.cue` CD-ROM image pair (the format most
burning software and ODE loaders like Xstation/PicoStation expect) via
mkpsxiso, alongside the raw `.psexe` executable. If you're building
locally, install mkpsxiso yourself and either make sure it's on your
`PATH`, or pass its location manually via `-DMKPSXISO_PATH=/path/to/mkpsxiso`
when configuring the project.

## SIO EXE loading

Open **Settings -> SIO Loader** on the PS1 and press **X**. The loader opens
immediately; **Circle** returns to the dashboard while it is still waiting
for the first command. The active implementation deliberately
keeps the hardware-proven receiver inside the dashboard while it waits for
the PC and receives the complete PS-EXE into the free region above the
dashboard. Only after the final checksum and `MORE` does a low-RAM handoff
stub copy the payload to its final address, clear the dashboard, and execute
it. A reset or power-cycle is required after execution.

Send a PS-EXE with UniROM's NoPS tool or the included compatible sender:

```text
python -m pip install pyserial
python tools/sio_send_exe.py COM3 path\to\program.exe
```

Replace `COM3` with the USB serial adapter's port. The link uses 115200 baud,
8 data bits, no parity, and 2 stop bits. The default sender mode uses UniROM's
`SEXE`/V2 handshake and checksum-corrected 2048-byte chunks; `--raw` exists
only for compatibility with older builds of this project.

## UniROM SIO loader findings and replication notes

This section records the reverse-engineering work performed against the
supplied `unirom_bin.exe` (UniROM 8.0.K) so the loader can be recreated in
another PS1 project without repeating the investigation.

### What was discovered

The original stalled implementation assumed that UniROM accepted:

```text
PS-X EXE header (2048 bytes) + raw program bytes
```

That is not the normal UniROM EXE-upload protocol. The PC tool first sends a
command and negotiates a protocol version. The actual V2 exchange is:

```text
PC   -> PS1   SEXE
PS1  -> PC    OKV2
PC   -> PS1   UPV2
PS1  -> PC    OKAY
PC   -> PS1   2048-byte PS-EXE header
PC   -> PS1   entry PC, load address, body size, whole-body checksum
PC   -> PS1   2048-byte body chunk
PS1  -> PC    CHEK
PC   -> PS1   chunk checksum
PS1  -> PC    MORE       # accepted
PS1  -> PC    ERR!       # retransmit the same chunk
```

The four metadata values and checksums are little-endian 32-bit integers.
Checksums are additive byte sums modulo `2^32` in the V2 protocol.

The supplied binary contains these useful landmarks after disassembly:

| Address | Finding |
| --- | --- |
| `0x801D9C7C` | SIO1 initialization |
| `0x801D9B98` | Blocking SIO1 byte receive |
| `0x801D9B48` | SIO receive acknowledge/control operation |
| `0x801DABE4` | `OKV2` / `UPV2` / `OKAY` negotiation |
| `0x801D7AB4` | `SEXE` command recognition and EXE-loader dispatch |
| `0x801D70F0` | 2048-byte receive/checksum/retry loop |

The binary's SIO1 setup is:

```c
SIO1_CTRL = 0x0040;
SIO1_BAUD = 0x0012;
SIO1_MODE = 0x00CE;  // 8 data bits, no parity, 2 stop bits
SIO1_CTRL = 0x0005;  // enable transmitter and receiver
```

The serial link is therefore 115200 baud, 8N2. At 8N2, a byte occupies
approximately 11 bit times, so ten chunks of 2048 bytes take roughly two
seconds. The receiver uses that interval for progress updates. It does
not redraw while unacknowledged bytes are arriving. The current receiver uses
the known-good dashboard renderer only while NoPS is blocked waiting for
`MORE`, approximately every eleven accepted chunks and at completion.

### PS-EXE structure and metadata

A standard PS-EXE begins with a 2048-byte header. The fields used by the
loader are:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| `0x00` | 8 | ASCII `PS-X EXE` |
| `0x10` | 4 | Entry PC |
| `0x14` | 4 | Initial `$gp` |
| `0x18` | 4 | Load address |
| `0x1C` | 4 | Header-declared text size |
| `0x30` | 4 | Stack base |
| `0x34` | 4 | Stack offset |

The NoPS EXE sender pads the complete file, not merely the declared text
section, to a 2048-byte boundary:

```python
padded_file = exe + bytes((-len(exe)) % 2048)
header = padded_file[:2048]
body = padded_file[2048:]
```

It then sends these values after the header:

```python
metadata = struct.pack(
    "<IIII",
    entry_pc,
    load_address,
    len(body),
    sum(body) & 0xffffffff,
)
```

The actual metadata body size is authoritative for the transfer. Do not
assume that the header's text-size field is the complete file length; NoPS
deliberately uses the padded file length minus `0x800`.

### Correct receive algorithm

A compatible receiver should follow this order:

1. Initialize SIO1 to 115200 8N2 and clear stale RX/error state.
2. Wait for either `SEXE` or, if legacy compatibility is required, a leading
   `P` from a raw PS-EXE stream.
3. For `SEXE`, send `OKV2` and wait for `UPV2`.
4. Draw the “Receiving header...” screen while the host is waiting.
5. Send `OKAY`.
6. Immediately read the 2048-byte header and four metadata words. Do not
   perform a VSync-synchronized redraw while these bytes are arriving.
7. Validate the PS-EXE magic, entry address, load address, RAM bounds, and
   maximum transfer size.
8. Receive into a staging range which does not overlap the running
   application. Do not change execution context, BIOS tables, or low-RAM
   ownership until the entire serial exchange has completed. For each chunk:

   ```text
   read exactly min(2048, remaining) bytes
   calculate additive byte checksum
   send CHEK
   read host checksum
   if checksum differs:
       send ERR!
       receive the same chunk again
   else:
       update progress
       send MORE
   ```

9. Check the whole-body checksum.
10. Stop hardware activity, flush the instruction cache, and jump using the
    header's entry PC, GP, and stack fields.

### Why the first version stalled

The old receiver read the first byte, immediately called a normal UI draw,
and only then continued reading the rest of the header. A normal frame waits
for VSync and starts a GPU DMA transfer. At 115200 baud, the PC continues
sending bytes while the CPU is waiting, and SIO1 has only a tiny receive FIFO.
The result is an overrun or a latched SIO error, which looks like a transfer
that detected the file but never progresses.

The V2 protocol solves this by giving the receiver explicit flow control:
after every chunk, the PS1 sends `CHEK` and the PC cannot continue until it
receives `MORE`. This creates a safe point for a VSync-synchronized progress
redraw.

For any protocol without flow control, use a very short non-VSync drawing path
or do not redraw until the transfer has ended. Never pause for a full VSync
while the sender is streaming unacknowledged bytes.

### SIO1 implementation details that matter

The receive acknowledge must happen before reading the data register. The
working order is:

```c
SIO1_CTRL |= 0x0010;  // acknowledge/clear the receive condition
byte = SIO1_DATA;     // then consume the byte
```

Doing this in the opposite order can leave an overrun or framing condition
latched and prevent the next `RXRDY` state from appearing.

The polling masks used by the supplied implementation are:

```c
#define SIO_STAT_TXRDY 0x05
#define SIO_STAT_RXRDY 0x02
```

For the current project, preserve the hardware-proven build's transmit test:

```c
while ((SIO1_STAT & 0x05) == 0) {
    /* wait until either transmit-ready indication is present */
}
SIO1_DATA = byte;
```

An attempted change required the masked value to equal `0x05`. Although that
resembled one disassembled UniROM polling loop, hardware testing did not fix
detection, and it diverged from the attached build that demonstrably
transfers. Both the dashboard TTY and SIO response writer now retain the
working nonzero test.

The SIO1 registers are uncached KSEG1 addresses:

```c
#define SIO1_DATA (*(volatile uint8_t  *) 0xBF801050)
#define SIO1_STAT (*(volatile uint8_t  *) 0xBF801054)
#define SIO1_MODE (*(volatile uint16_t *) 0xBF801058)
#define SIO1_CTRL (*(volatile uint16_t *) 0xBF80105A)
#define SIO1_BAUD (*(volatile uint16_t *) 0xBF80105E)
```

If the application also installs a TTY redirect on SIO1, the loader should
avoid printing diagnostic text during the transfer. Any unsolicited bytes
can be mistaken for a command or response by a sliding four-byte protocol
parser. Clear stale RX data before arming the loader, and reset/acknowledge
the SIO state before waiting for `SEXE`.

The receiver should not offer a user cancel action after the host has started
sending a command or chunk. If the console abandons the exchange without
sending the expected response, the PC remains blocked waiting for `OKAY`,
`MORE`, or `ERR!`. Cancellation is safe while waiting for the first command
and after a transfer has stopped.

### RAM clearing, staging, and launch safety

An in-application receiver must not write directly to its final address.
Common PS-EXEs load around `0x80010000`, which overlaps the application's code
and data. A relocated pre-transfer receiver was tested, but it broke serial
detection on hardware. The current solution retains the exact known-good
receiver and stages the complete payload above `_imageEnd`. After the final
checksum, a position-independent handoff stub performs an overlap-safe copy
and jumps to the received program.

The embedded-image handoff is intentionally the stable path from the
provided build:

1. Silence SPU playback.
2. Drain/stop DMA channel 2 and all other DMA channels.
3. Reset the GPU, mask interrupts, clear COP0 breakpoints, and disable CPU
   interrupts.
4. Copy the embedded payload with memmove semantics.
5. Flush the instruction cache.
6. Jump with the PS-EXE entry PC, GP, and stack values.

The SIO path uses the same hardware cleanup only after the complete transfer,
then copies its staged payload through a 42-word trampoline at
`0x8000C800`, with parameters at `0x8000C7C0`. This is deliberately separate
from the embedded Fast Boot path so a new BIOS-Exec or RAM-wipe experiment
cannot break the known-good CD loader again.

Do not use the BIOS kernel allocation region as a staging buffer. The staged
payload is placed above `_imageEnd`; the only low-RAM scratch used after the
transfer is the 42-word trampoline at `0x8000C800`.

A stock 2 MB PS1 should be treated conservatively as ending at physical
address `0x00200000`. Always check both the physical load start and the
`start + size` end for overflow before accepting a transfer.

### Why the proven `cdloader.exe` path matters

The embedded loader is a normal PS-EXE, not a special BIOS-only format. Reading
its header gives:

```text
file size       0x4000 (16 KiB)
entry PC        0x801EA324
load address    0x801EA324
payload size    0x3800
initial SP      0x801FFF00
```

Its first instructions immediately move the stack to `0x801FFEF0`, clear its
own BSS, and then run its normal GPU setup. That explains why a high load
address appears to work even when a dashboard is large: the loader's payload
does not overwrite the dashboard until the handoff has already finished.

The supplied UniROM image follows the same PS-EXE contract:

```text
file size       0x21800 (137216 bytes)
entry PC        0x801D0000
load address    0x801D0000
payload size    0x21000
initial SP      0x801FFF00
```

At its entry it clears its own BSS (`0x801F1000` through `0x801F8A30`), sets
`a0` and `a1` to zero, and jumps to its startup routine at `0x801D0038`.
Therefore the reliable way to launch it is to reproduce the ordinary PS-EXE
register contract, not to call an internal UniROM function or to leave the
dashboard resident.

The handoff deliberately synthesizes the same PC/GP/SP register state used by
the stable build and jumps directly after flushing the cache. This is the
contract that previously made Fast Boot work.

### Why the July 2026 scratch-loader build failed

Two independent regressions produced the reported symptoms.

#### NoPS detected nothing after `LOADING EXE`

NoPS sends `SEXE` and then waits for `OKAY`. If it receives `OKV2`, it sends
`UPV2` but continues waiting for that final `OKAY`; it does not send the
header early. The relocated low-RAM receiver never produced a valid complete
response on the tested console. The visible `LOADING EXE` screen was only the
last dashboard frame and was not proof that the relocated code had started.

The correction is intentionally conservative: all SIO setup, FIFO draining,
`SEXE` detection, response polling, V2 negotiation, header/metadata reading,
chunk checks, retries, and progress timing were restored from the attached
hardware-working build. No BIOS reconstruction, RAM clearing, low-RAM copy,
or execution-context change now occurs before the final accepted chunk.

The restored receiver now:

1. uses the hardware-proven nonzero `SIO1_STAT & 0x05` transmit test;
2. sends `OKV2`, reads `UPV2`, and sends `OKAY`;
3. receives the header and metadata without invoking dashboard code;
4. updates progress only while NoPS is stopped waiting for `MORE`;
5. uses `CHEK`/`MORE`/`ERR!` exactly as NoPS expects; and
6. starts the received file with the header's PC/GP/SP values.

#### Black screen after a completed SIO transfer

The transfer completing proves the NoPS receiver and staging range are
working. The regression was in the experimental post-transfer BIOS-Exec
scratch stub: it added BIOS reconstruction, a copied Exec record, a whole-RAM
wipe, and a new launch path that were not present in the stable build.

The current handoff restores the proven direct copy/jump for Fast Boot and
embedded UniROM. SIO uses a separate 42-word trampoline only after the final
checksum and `MORE`; it copies the staged body, flushes the cache, and jumps
with the header's PC/GP/SP values. No BIOS reconstruction or RAM wipe occurs
before the transfer finishes.

#### What the attached CD loader proved

The supplied `cdloader.exe` is byte-identical to `assets/cdloader.exe`
(SHA-256
`63C63FB5445C0191FC0D2C2E5B7BAE9E1E662E0547D7695F67EC64F16E187DB6`).
Its source archive shows two important behaviors:

1. `main()` immediately calls `bios_reinitialize()` for the CD-loader's own
   fresh-program environment. That is not called by the dashboard before
   Fast Boot; adding it there was part of the failed experiment.
2. Its final executable launch uses:

   ```c
   EnterCriticalSection();
   FlushCache();
   DoExecute(&exe_header->offsets, 0, 0);
   ```

   Its `DoExecute` wrapper also zeros `$s1`-`$s6` before jumping to A0(43h).
   The stable embedded-image and SIO handoffs use the equivalent direct
   PC/GP/SP contract.

The supplied UniROM hash is
`CFA37622B7587CC6A4B1A9987C679FDDBCF69B587DC980B156351F5F91D9CFFB`.
These hashes make it possible to tell whether later tests are using the exact
binaries analyzed here.

### Verification checklist for future embedded tools

Before adding another tool, verify all of the following:

- The file begins with `PS-X EXE`, is at least `0x800` bytes, and its declared
  destination range stays inside stock 2 MB RAM.
- The destination does not overlap low handoff RAM
  (`0x8000C000-0x8000D800`) or the BIOS kernel region at
  `0x8000E000-0x80010000`.
- Keep BIOS/device ownership unchanged until serial reception completes;
  preserve the stable direct launch contract before experimenting with BIOS
  table reconstruction.
- GPU DMA channel 2 is drained and each DMA CHCR is stopped; clearing only
  DPCR can strand a channel busy.
- COP0 breakpoints (`DCIC`, `BDA`, `BDAM`) are cleared.
- The target body is copied before surrounding RAM is wiped.
- The final cache flush happens after the last code write.
- A standard PS-EXE is entered with its header PC/GP/SP contract after a cache
  flush.
- Test both a low-load executable (`0x80010000`) and a high-load executable,
  and test an executable that uses BIOS stdout immediately at startup.

The handoff object was compiled with the project's R3000 flags. The SIO
trampoline is 42 instructions (`0xA8` bytes) and is copied to `0x8000C800` only after the
transfer has completed.

### PC sender example

The included sender is
[`tools/sio_send_exe.py`](tools/sio_send_exe.py). It implements the V2
protocol by default:

```powershell
python -m pip install pyserial
python tools/sio_send_exe.py COM3 myprogram.exe
```

Linux/macOS example:

```bash
python3 -m pip install pyserial
python3 tools/sio_send_exe.py /dev/ttyUSB0 myprogram.exe
```

Useful options:

```text
--write-size 256   split writes for unreliable USB adapters
--delay 0.001      pause between split writes
--timeout 8        response timeout in seconds
--raw              use the old unframed header/body stream
```

`--raw` is only for older versions of this project that did not implement the
UniROM handshake. It is not the preferred mode for UniROM-compatible
transfers because it has no `CHEK`/`MORE` back-pressure.

### How to reverse-engineer another UniROM build

The following workflow is repeatable for a different UniROM binary:

1. Confirm that the file is a PS-EXE and record its entry point, load address,
   text size, and stack fields.
2. Extract the payload after the first `0x800` bytes.
3. Disassemble the MIPS little-endian payload at its runtime load address.
   Example:

   ```text
   mipsel-none-elf-objdump -D -b binary -m mips:isa32r2 -EL ^
       --adjust-vma=0x801D0000 unirom_payload.bin > unirom_disasm.txt
   ```

4. Search the disassembly for protocol strings:

   ```text
   OKV2
   UPV2
   OKAY
   SEXE
   CHEK
   MORE
   ERR!
   ```

5. Find the callers and cross-references of those strings. Record:
   - command markers;
   - response markers;
   - integer field order;
   - chunk size;
   - checksum algorithm;
   - retry response;
   - destination and maximum RAM checks.
6. Compare the result with the PC-side NoPS implementation before writing a
   new receiver.
7. Test with a fake serial peer that asserts every command and reconstructs
   the received body before testing on hardware.

The supplied UniROM payload was 135168 bytes and its main PS-EXE image loaded
at `0x801D0000`. Those values are observations for that specific binary, not
universal constants for every UniROM release.

### Compatibility and protocol versions

The published NoPS implementation documents V2 as the individual
per-2048-byte checksum protocol. Newer UniROM builds may advertise V3 and use
a different checksum algorithm. A receiver that is intended to support
multiple releases should inspect the `OKVn` response and implement each
version separately rather than treating V3 as V2.

This project intentionally implements the V2 behavior required by the
supplied UniROM 8.0.K image. The relevant external references are:

- [UniROM advanced documentation](https://unirom.github.io/advanced/)
- [NOTPSXSerial repository](https://github.com/JonathanDotCel/NOTPSXSerial)
- [NoPS EXE transfer implementation](https://github.com/JonathanDotCel/NOTPSXSerial/blob/master/TransferLogic.cs)
- [PSX-SPX BIOS `Exec` reference](https://psx-spx.consoledev.net/kernelbios/#a43h-execheaderbuf-param1-param2)
- [PSX-SPX serial interface reference](https://psx-spx.consoledev.net/serialinterfacesio/)
- [PS1 bare-metal build/runtime reference](https://github.com/spicyjpeg/ps1-bare-metal)
- [tonyhax International source](https://github.com/alex-free/tonyhax)

## See also

- [psx-spx](https://psx-spx.consoledev.net/), the main hardware reference
  used throughout this project.
- If you need help or wish to discuss PS1 homebrew development more in
  general, you may want to check out the
  [PSX.Dev Discord server](https://discord.gg/QByKPpH).
