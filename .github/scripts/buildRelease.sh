#!/bin/bash

ROOT_DIR="$(pwd)"
PROJECT_DIR="$ROOT_DIR/PSX-iTests"
TOOLCHAIN_DIR="$ROOT_DIR/gcc-mipsel-none-elf"

## Build project

cmake --preset release -DTOOLCHAIN_PATH="$TOOLCHAIN_DIR" "$PROJECT_DIR" \
	|| exit 1
cmake --build "$PROJECT_DIR/build" \
	|| exit 1

RELEASE_NAME="$(
	ls "$PROJECT_DIR/build" |
	grep -E -o '^PSX-iTests-[0-9]+\.[0-9]+\.[0-9]+' |
	head -n 1
)"

mkdir -p "$ROOT_DIR/$RELEASE_NAME"
cd "$ROOT_DIR/$RELEASE_NAME"
cp \
	"$PROJECT_DIR/build/$RELEASE_NAME.bin" \
	"$PROJECT_DIR/build/$RELEASE_NAME.cue" \
	"$PROJECT_DIR/build/$RELEASE_NAME.psexe" \
	"$PROJECT_DIR/build/readme.txt" \
	.

## Package release

zip -9 -r "$ROOT_DIR/$RELEASE_NAME.zip" . \
	|| exit 3

#cd "$ROOT_DIR"
#rm -rf "$RELEASE_NAME"
