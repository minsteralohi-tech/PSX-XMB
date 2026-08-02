#!/usr/bin/env python3
"""Convert the PSX_ID.txt game list into a compact binary lookup table.

    python tools/makeGameDb.py assets/PSX_ID.txt assets/gamedb.dat

INPUT FORMAT

The source list is a hand-maintained text file, so it needs a real parser
rather than a regex per line:

    SLUS-00553<TAB> ALUNDRA                 normal entry
    SLUS-01201                              a bare ID: belongs to the NEXT
    SLUS-01377<TAB> ALONE IN THE DARK ...   named entry (a multi-disc game)
    [ A ]  -  NTSC-U                        section header, ignored
     Includes: GDI + NOD discs              continuation note, ignored
    SLUS-0XXXX<TAB> STARCON (PROTOTYPE)     placeholder ID, ignored

OUTPUT FORMAT

Everything is little-endian, and the entry table is sorted by key so the
console can binary-search it:

    +0   magic  "PSXG"
    +4   u16    entry count
    +6   u16    reserved (0)
    +8   entries[count]:
             u32 key      packed ID, see packKey()
             u16 offset   byte offset into the name blob
    ...  name blob: NUL-terminated ASCII, deduplicated

Keys pack a 4-character prefix and a number into 32 bits, so a lookup is an
integer compare rather than a string compare - no strcmp per probe, and no
need to store the ID text at all.
"""

import argparse
import re
import struct
import sys
from pathlib import Path

# Prefixes worth carrying. LSP (Lightspan educational discs) uses a 6-digit
# number that does not fit the scheme and is not something anyone is likely to
# boot on this dashboard, so it is skipped rather than special-cased.
PREFIXES = ["SLUS", "SCUS", "SLES", "SCES", "SLPS", "SCPS", "SLPM", "SCAJ"]

ENTRY_RE = re.compile(r"^([A-Z]{4})-(\d{5})(?:\s+(.*))?$")


def pack_key(prefix, number):
    """Pack a disc ID into 32 bits: prefix index in the top 5, number below.

    Numbers run to 94964, which needs 17 bits, leaving plenty of room."""
    return (PREFIXES.index(prefix) << 27) | (number & 0x07FFFFFF)


def parse(path):
    entries = {}
    pending = []
    skipped = 0

    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.replace("\r", "").strip()

        if not line:
            continue

        # Section headers and continuation notes.
        if line.startswith("[") or line.lower().startswith("includes:"):
            pending.clear()
            continue

        match = ENTRY_RE.match(line)

        if not match:
            # Placeholder IDs, LSP discs, stray notes.
            skipped += 1
            pending.clear()
            continue

        prefix, number, name = match.group(1), int(match.group(2)), match.group(3)

        if prefix not in PREFIXES:
            skipped += 1
            pending.clear()
            continue

        if not name or not name.strip():
            # A bare ID belongs to the next named entry - multi-disc release.
            pending.append((prefix, number))
            continue

        name = " ".join(name.split())

        for pre, num in pending + [(prefix, number)]:
            entries[pack_key(pre, num)] = name

        pending.clear()

    return entries, skipped


def build(entries):
    # Deduplicate names: multi-disc releases and re-releases share strings.
    blob = bytearray()
    offsets = {}

    for name in sorted(set(entries.values())):
        offsets[name] = len(blob)
        blob += name.encode("ascii", errors="replace") + b"\0"

    if len(blob) > 0xFFFF:
        sys.exit(f"name blob is {len(blob)} bytes, too big for u16 offsets")

    out = bytearray()
    out += b"PSXG"
    out += struct.pack("<HH", len(entries), 0)

    for key in sorted(entries):
        out += struct.pack("<IH", key, offsets[entries[key]])

    out += blob
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source")
    parser.add_argument("output")
    args = parser.parse_args()

    entries, skipped = parse(Path(args.source))

    if not entries:
        sys.exit("no entries parsed - has the source format changed?")

    data = build(entries)
    Path(args.output).write_bytes(data)

    names = len(set(entries.values()))
    print(f"{args.output}: {len(entries)} IDs, {names} unique names, "
          f"{len(data)} bytes ({skipped} source lines skipped)")


if __name__ == "__main__":
    main()
