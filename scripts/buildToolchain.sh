#!/bin/bash

ROOT_DIR="$(pwd)"
BINUTILS_VERSION="2.46.0"
GCC_VERSION="16.1.0"
# Match the number of build jobs to the available cores. Oversubscribing (e.g.
# a fixed -j4 on the 2-core GitHub runners) makes GCC's largest translation
# units contend for the ~7 GB of RAM and can thrash into swap or OOM, which
# looks like a silent hang. nproc keeps it right-sized on any runner.
NUM_JOBS="$(nproc 2>/dev/null || echo 2)"

# GNU's mirror redirector (ftpmirror.gnu.org) picks a essentially random
# real mirror on every request, and individual mirrors occasionally return
# transient errors (502/503, timeouts) under load. wget's own --tries only
# retries against that one bad connection; retrying the whole command after
# a short pause re-resolves the redirector and usually lands on a different,
# healthy mirror. Without this, a single flaky mirror could fail the entire
# CI job on a cache miss.
retry_download() {
	local attempt=1
	local max_attempts=5
	while [ $attempt -le $max_attempts ]; do
		if "$@"; then
			return 0
		fi
		echo "Download attempt $attempt/$max_attempts failed, retrying in $((attempt * 10))s..."
		sleep $((attempt * 10))
		attempt=$((attempt + 1))
	done
	echo "All $max_attempts download attempts failed." >&2
	return 1
}

# Tries each mirror in turn (with retries per mirror via retry_download)
# before giving up - so a redirector having a bad day doesn't fail the
# whole job as long as one alternate mirror is healthy.
download_from_mirrors() {
	local out_file="$1"
	shift
	for url in "$@"; do
		echo "Trying: $url"
		if retry_download wget -O "$out_file" \
			--tries=3 --timeout=30 --waitretry=10 --retry-connrefused "$url"
		then
			return 0
		fi
		echo "Mirror failed, trying next one if available: $url"
	done
	echo "All mirrors failed for $out_file" >&2
	return 1
}

if [ $# -eq 2 ]; then
	PACKAGE_NAME="$1"
	TARGET_NAME="$2"
	BUILD_OPTIONS=""
elif [ $# -eq 3 ]; then
	PACKAGE_NAME="$1"
	TARGET_NAME="$2"
	BUILD_OPTIONS="--build=x86_64-linux-gnu --host=$3"
else
	echo "Usage: $0 <package name> <target triplet> [host triplet]"
	exit 0
fi

## Download binutils and GCC

if [ ! -d binutils-$BINUTILS_VERSION ]; then
	download_from_mirrors "binutils-$BINUTILS_VERSION.tar.xz" \
		"https://ftpmirror.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz" \
		"https://mirrors.kernel.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz" \
		|| exit 1
	tar Jxf binutils-$BINUTILS_VERSION.tar.xz \
		|| exit 1

	rm -f binutils-$BINUTILS_VERSION.tar.xz
fi

if [ ! -d gcc-$GCC_VERSION ]; then
	download_from_mirrors "gcc-$GCC_VERSION.tar.xz" \
		"https://ftpmirror.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz" \
		"https://mirrors.kernel.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz" \
		|| exit 1
	tar Jxf gcc-$GCC_VERSION.tar.xz \
		|| exit 1

	cd gcc-$GCC_VERSION
	retry_download contrib/download_prerequisites \
		|| exit 1

	cd ..
	rm -f gcc-$GCC_VERSION.tar.xz
fi

## Build binutils

mkdir -p binutils-build
cd binutils-build

../binutils-$BINUTILS_VERSION/configure \
	--prefix="$ROOT_DIR/$PACKAGE_NAME" \
	$BUILD_OPTIONS \
	--target=$TARGET_NAME \
	--with-float=soft \
	--disable-docs \
	--disable-nls \
	--disable-werror \
	|| exit 2
make -j $NUM_JOBS \
	|| exit 2
make install-strip \
	|| exit 2

cd ..
rm -rf binutils-build

## Build GCC

mkdir -p gcc-build
cd gcc-build

../gcc-$GCC_VERSION/configure \
	--prefix="$ROOT_DIR/$PACKAGE_NAME" \
	$BUILD_OPTIONS \
	--target=$TARGET_NAME \
	--with-float=soft \
	--disable-docs \
	--disable-nls \
	--disable-werror \
	--disable-libada \
	--disable-libssp \
	--disable-libquadmath \
	--disable-threads \
	--disable-libgomp \
	--disable-libstdcxx-pch \
	--disable-hosted-libstdcxx \
	--enable-languages=c \
	--without-isl \
	--without-headers \
	--with-gnu-as \
	--with-gnu-ld \
	|| exit 3
make -j $NUM_JOBS \
	|| exit 3
make install-strip \
	|| exit 3

cd ..
rm -rf gcc-build

## Package toolchain

#cd $PACKAGE_NAME

#zip -9 -r ../$PACKAGE_NAME-$GCC_VERSION.zip . \
#	|| exit 4

#cd ..
#rm -rf $PACKAGE_NAME
