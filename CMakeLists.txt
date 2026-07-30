# PSX-iTests - build script adapted from ps1-ram-tester / ps1-bare-metal
# by spicyjpeg (https://github.com/spicyjpeg), used here under the MIT
# license. See LICENSE for the full original license text.
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

set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/cmake/toolchain.cmake")

project(
	PSX-iTests
	LANGUAGES    C ASM
	VERSION      1.0.0
	DESCRIPTION  "PlayStation 1 basic CPU/GPU/SPU functionality test tool"
	HOMEPAGE_URL "https://github.com/spicyjpeg/ps1-bare-metal"
)

include(cmake/options.cmake)
include(cmake/setup.cmake)
include(cmake/tools.cmake)

## Standard library

add_library(
	common OBJECT
	src/libc/clz.s
	#src/libc/crt0.c
	#src/libc/cxxsupport.cpp
	#src/libc/malloc.c
	src/libc/misc.c
	src/libc/setjmp.s
	src/libc/string.c
	src/libc/string.s
	src/ps1/cache.s
	src/vendor/printf.c
)
target_include_directories(
	common PUBLIC
	src
	src/libc
)
link_libraries(common)

## Main executable

addPS1Executable(
	"${RELEASE_NAME}"
	"${RELEASE_INFO}"
	src/common/gpu.c
	src/common/reboot.c
	src/common/reboot.s
	src/common/sio0.c
	src/common/spu.c
	src/main/cd_player.c
	src/main/cpu_bench.c
	src/main/console_info.c
	src/main/memcard.c
	src/main/crt0.s
	src/main/font.c
	src/main/gpu_colorbars.c
	src/main/gpu_cube.c
	src/main/main.c
	src/main/mainmenu.c
	src/main/modals.c
	src/main/pad_test.c
	src/main/ramconfig.c
	src/main/ramtester.c
	src/main/renderer.c
	src/main/sound.c
	src/main/spu_channel_test.c
	src/main/test.c
	src/main/trig.c
	src/main/ui.c
	src/main/xmb_bg.c
	src/main/fastboot.c
	src/main/icon.c
	src/main/model_test.c
	src/main/xmb_menu.c
	src/main/tty_serial.c
	src/main/bios_calls.s
	src/main/hud.c
	src/main/badge.c
	src/main/sio_loader.c
	src/main/sio_staged_stub.s
	src/main/music_settings.c
	src/main/unirom_launch.c
	src/main/handoff.c
)
target_compile_options(
	"${RELEASE_NAME}" PRIVATE
	-Wall
	-Wextra
)
target_compile_definitions(
	"${RELEASE_NAME}" PRIVATE
	$<$<BOOL:${ENABLE_LOGGING}>:ENABLE_LOGGING=1>
	VERSION="${PROJECT_VERSION}"
)

convertImage(assets/background.png 4 assets/bgTexture.dat assets/bgPalette.dat)
addBinaryFile("${RELEASE_NAME}" bgTexture "${PROJECT_BINARY_DIR}/assets/bgTexture.dat")
addBinaryFile("${RELEASE_NAME}" bgPalette "${PROJECT_BINARY_DIR}/assets/bgPalette.dat")

convertImage(assets/font.png 4 assets/fontTexture.dat assets/fontPalette.dat)
addBinaryFile("${RELEASE_NAME}" fontTexture "${PROJECT_BINARY_DIR}/assets/fontTexture.dat")
addBinaryFile("${RELEASE_NAME}" fontPalette "${PROJECT_BINARY_DIR}/assets/fontPalette.dat")

convertImage(assets/icons_cat.png 4 assets/catTexture.dat assets/catPalette.dat)
addBinaryFile("${RELEASE_NAME}" catTexture "${PROJECT_BINARY_DIR}/assets/catTexture.dat")
addBinaryFile("${RELEASE_NAME}" catPalette "${PROJECT_BINARY_DIR}/assets/catPalette.dat")

convertImage(assets/icons_item.png 4 assets/itemTexture.dat assets/itemPalette.dat)
addBinaryFile("${RELEASE_NAME}" itemTexture "${PROJECT_BINARY_DIR}/assets/itemTexture.dat")
addBinaryFile("${RELEASE_NAME}" itemPalette "${PROJECT_BINARY_DIR}/assets/itemPalette.dat")

# Sun/lava surface texture for the Nebula 2 theme's hero planet (8bpp: a
# photographic gradient texture needs more than 16 colours to avoid banding).
# Pre-resized to 256x128 (fits one 8bpp texture page) and pre-quantized to
# <=256 colours - see assets/sun.png's provenance in xmb_bg.c's comments.
convertImage(assets/sun.png 8 assets/sunTexture.dat assets/sunPalette.dat)
addBinaryFile("${RELEASE_NAME}" sunTextureData "${PROJECT_BINARY_DIR}/assets/sunTexture.dat")
addBinaryFile("${RELEASE_NAME}" sunPaletteData "${PROJECT_BINARY_DIR}/assets/sunPalette.dat")

# Lava, Earth and Moon surface textures for the Nebula 3 theme's textured
# roaming planets - same 256x128, pre-quantized <=256 colour provenance as
# assets/sun.png above (see xmb_bg.c's comments on drawTexturedPlanet()).
convertImage(assets/lava.png 8 assets/lavaTexture.dat assets/lavaPalette.dat)
addBinaryFile("${RELEASE_NAME}" lavaTextureData "${PROJECT_BINARY_DIR}/assets/lavaTexture.dat")
addBinaryFile("${RELEASE_NAME}" lavaPaletteData "${PROJECT_BINARY_DIR}/assets/lavaPalette.dat")

convertImage(assets/earth.png 8 assets/earthTexture.dat assets/earthPalette.dat)
addBinaryFile("${RELEASE_NAME}" earthTextureData "${PROJECT_BINARY_DIR}/assets/earthTexture.dat")
addBinaryFile("${RELEASE_NAME}" earthPaletteData "${PROJECT_BINARY_DIR}/assets/earthPalette.dat")

convertImage(assets/moon.png 8 assets/moonTexture.dat assets/moonPalette.dat)
addBinaryFile("${RELEASE_NAME}" moonTextureData "${PROJECT_BINARY_DIR}/assets/moonTexture.dat")
addBinaryFile("${RELEASE_NAME}" moonPaletteData "${PROJECT_BINARY_DIR}/assets/moonPalette.dat")

# Three selectable SFX sets - PSP (the original set), MGS, and PS4. All 9
# samples (3 sets x 3 sounds) are small enough to stay resident in SPU RAM
# simultaneously (~130 KB combined, see initSound()), so switching sets is
# just changing which offsets playScrollSound()/playConfirmSound()/
# playCancelSound() use - no re-upload/DMA needed, unlike selectBGM().
addBinaryFile("${RELEASE_NAME}" sfxPspNavSound     assets/nav.vag)
addBinaryFile("${RELEASE_NAME}" sfxPspConfirmSound assets/confirm.vag)
addBinaryFile("${RELEASE_NAME}" sfxPspCancelSound  assets/cancel.vag)
addBinaryFile("${RELEASE_NAME}" sfxMgsNavSound     assets/mgs_nav.vag)
addBinaryFile("${RELEASE_NAME}" sfxMgsConfirmSound assets/mgs_confirm.vag)
addBinaryFile("${RELEASE_NAME}" sfxMgsCancelSound  assets/mgs_cancel.vag)
addBinaryFile("${RELEASE_NAME}" sfxPs4NavSound     assets/ps4_nav.vag)
addBinaryFile("${RELEASE_NAME}" sfxPs4ConfirmSound assets/ps4_confirm.vag)
addBinaryFile("${RELEASE_NAME}" sfxPs4CancelSound  assets/ps4_cancel.vag)
addBinaryFile("${RELEASE_NAME}" bgm1Sound assets/ps3xmb.vag)
addBinaryFile("${RELEASE_NAME}" bgm2Sound assets/ps4xmb.vag)
addBinaryFile("${RELEASE_NAME}" bgm3Sound assets/sanctuary.vag)
addBinaryFile("${RELEASE_NAME}" spuTestSound assets/spu.vag)

# The Fast Boot loader (cdloader.exe) embedded as a raw PS-EXE blob, launched
# by launchLoader() in fastboot.c. Kept self-contained inside our .exe - no
# separate file on the disc. The stable shared handoff copies the image,
# flushes the cache, and starts it with the PS-EXE PC/GP/SP contract.
addBinaryFile("${RELEASE_NAME}" cdloaderExe assets/cdloader.exe)

# Real UniROM 8.0 build, embedded the same way for the Tools -> "UniROM 8.0"
# entry (launchUniROM() in unirom_launch.c). Chain-loading the genuine
# UniROM binary sidesteps any risk in this project's own hand-rolled SIO
# Loader reimplementation - once real UniROM is running, its own serial EXE
# loading is exactly what it always is.
addBinaryFile("${RELEASE_NAME}" uniromExe assets/unirom_bin.exe)

## CD-ROM image

configure_file(assets/cdreadme.txt  readme.txt      NEWLINE_STYLE CRLF)
configure_file(LICENSE              license.txt     NEWLINE_STYLE CRLF)
configure_file(assets/system.cnf    system.cnf      NEWLINE_STYLE CRLF)
configure_file(assets/isoproject.xml isoproject.xml NEWLINE_STYLE LF)

# Builds the CD-ROM image with mkpsxiso (https://github.com/Lameguy64/mkpsxiso)
# instead of the previous custom Python builder - the custom builder never
# wrote a SYSTEM.CNF at all (silently non-bootable on real hardware) and
# mkpsxiso is the same proven, widely-used tool most PS1 homebrew projects
# rely on. NOTE: this build does not inject Sony license/logo data (the
# region text + boot logo splash) - the confirmed-working reference
# project this was adapted from didn't use it either, and it's a cosmetic
# omission only, not a functional one (the disc still boots fine without
# it).
if(NOT MKPSXISO_PATH)
	message(FATAL_ERROR
		"mkpsxiso not found. Install it and make sure it's on PATH, or "
		"pass -DMKPSXISO_PATH=/path/to/mkpsxiso when configuring."
	)
endif()

add_custom_command(
	COMMAND
		"${MKPSXISO_PATH}"
		isoproject.xml
	OUTPUT
		"${RELEASE_NAME}.bin"
		"${RELEASE_NAME}.cue"
	DEPENDS
		isoproject.xml
		readme.txt
		license.txt
		system.cnf
		"${RELEASE_NAME}"
	COMMENT "Building CD-ROM BIN/CUE image with mkpsxiso"
	VERBATIM
)

add_custom_target(
	cdrom ALL
	DEPENDS "${RELEASE_NAME}.bin"
)
