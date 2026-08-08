# PSX-iTests - adapted from ps1-ram-tester by spicyjpeg
# (https://github.com/spicyjpeg), used here under the MIT license. See
# LICENSE for the full original license text.
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
# REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
# AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
# INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
# LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
# OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
# PERFORMANCE OF THIS SOFTWARE.

cmake_minimum_required(VERSION 3.25)

## External tools

find_program(
	MKPSXISO_PATH mkpsxiso
	DOC "Path to the mkpsxiso tool, used to build the CD-ROM image"
)

## Release information

set(
	RELEASE_INFO "${PROJECT_NAME} ${PROJECT_VERSION}"
	CACHE STRING "Executable description and version string, placed in the \
executable header (optional)"
)
set(
	RELEASE_NAME "${PROJECT_NAME}-${PROJECT_VERSION}"
	CACHE STRING "CD-ROM image and release package file name"
)

string(TOUPPER "${RELEASE_NAME}" _cdVolumeName)
string(REGEX REPLACE "[^0-9A-Z_]" "_" _cdVolumeName "${_cdVolumeName}")

set(
	CD_VOLUME_NAME "${_cdVolumeName}"
	CACHE STRING "CD-ROM image volume label"
)

## Compile-time options

set(
	ENABLE_LOGGING OFF
	CACHE BOOL "Enable debug logging to serial port (SIO1)"
)

# Artemio Urbina's 240p Test Suite (assets/240p.exe), embedded and launched
# from Hardware Tests. OFF by default because at 395,264 bytes it is what
# pushed this dashboard's total RAM footprint over the ceiling imposed by
# loading it over serial under UniROM - see the UNIROM_CEILING check in
# cmake/executable.ld for the exact numbers and why they are what they are.
#
# Nothing else was removed to make room: this one file is the whole of the
# 161 KB that had to go. assets/240p.exe stays in the repository, the launch
# code stays in src/main/launch_ui.c, and the menu entry stays visible (greyed
# out) in Hardware Tests:
#
#     cmake -B build -DEMBED_240P_SUITE=ON
#
# Turning it ON also waives the serial-loading check in cmake/executable.ld,
# because such a build is for a CD or ODE where the whole 2 MB is genuinely
# ours.
#
# BE AWARE, AS OF THE TEXTURED CONSOLE MODELS: ON does not currently link. The
# pose tool's original-PlayStation model carries a 256x256 8bpp texture (64 KB
# of the 65 KB those two models added), and with the 240p suite back in, the
# build overflows stock 2 MB RAM by 34,840 bytes - a real hardware limit, not
# the UniROM check. Re-enabling it therefore needs roughly 35 KB freed as well.
# The cheapest 35 KB is that texture: dropping it to 128x128 in
# tools/glb2console.py's MODELS saves 49 KB and costs the SONY/PlayStation
# lettering its crispness.
set(
	EMBED_240P_SUITE OFF
	CACHE BOOL "Embed the 240p Test Suite (needs 395 KB; breaks serial loading \
under UniROM - see cmake/executable.ld)"
)
