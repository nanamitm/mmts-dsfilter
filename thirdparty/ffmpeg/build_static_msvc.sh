#!/bin/sh
# Build the static FFmpeg libraries vendored in this directory.
#
# Run from an MSYS shell whose PATH already has the MSVC x64 tools (cl, link,
# lib) - see build_static_msvc.cmd - and that provides make and nasm.
#
#   build_static_msvc.sh <ffmpeg-source-dir> [build-dir]
#
# Only what the 22.2ch AAC LATM -> PCM path in MmtTlvSplitter uses is enabled:
# the aac and aac_latm decoders, libavcodec, libavutil and libswresample. No
# --enable-gpl, --enable-version3 or --enable-nonfree, so the result stays
# LGPL 2.1+.
#
# One set of libraries is built per MSVC runtime so that they match the
# mmts-dsfilter configurations:
#   lib/MD   Release  (/MD)
#   lib/MDd  Debug    (/MDd)

set -e

if [ -z "$1" ]; then
	echo "usage: $0 <ffmpeg-source-dir> [build-dir]" >&2
	exit 1
fi

to_unix() {
	if command -v cygpath >/dev/null 2>&1; then cygpath -u "$1"; else echo "$1"; fi
}

SRC_DIR=$(cd "$(to_unix "$1")" && pwd)
OUT_DIR=$(cd "$(dirname "$(to_unix "$0")")" && pwd)
BUILD_ROOT=$(to_unix "${2:-$SRC_DIR/../build}")
mkdir -p "$BUILD_ROOT"
BUILD_ROOT=$(cd "$BUILD_ROOT" && pwd)

CRTS=${CRTS:-MD MDd}
for CRT in $CRTS; do
	BUILD_DIR=$BUILD_ROOT/$CRT
	rm -rf "$BUILD_DIR"
	mkdir -p "$BUILD_DIR"
	cd "$BUILD_DIR"

	"$SRC_DIR/configure" \
		--toolchain=msvc \
		--arch=x86_64 \
		--target-os=win64 \
		--prefix="$BUILD_DIR/install" \
		--enable-static \
		--disable-shared \
		--disable-programs \
		--disable-doc \
		--disable-autodetect \
		--disable-network \
		--disable-everything \
		--disable-avdevice \
		--disable-avformat \
		--disable-avfilter \
		--disable-swscale \
		--enable-avcodec \
		--enable-avutil \
		--enable-swresample \
		--enable-decoder=aac,aac_latm \
		--extra-cflags="-$CRT"

	make -j"$(nproc)"
	make install

	mkdir -p "$OUT_DIR/lib/$CRT"
	# Not "LIB": that is the MSVC linker's library search path.
	for FF_LIB in avcodec avutil swresample; do
		cp "$BUILD_DIR/install/lib/$FF_LIB.lib" "$OUT_DIR/lib/$CRT/$FF_LIB.lib"
	done
done

rm -rf "$OUT_DIR/include"
# The headers do not depend on the runtime; take them from the first build.
set -- $CRTS
cp -r "$BUILD_ROOT/$1/install/include" "$OUT_DIR/include"
rm -rf "$OUT_DIR/include/libavdevice" "$OUT_DIR/include/libavformat" \
	"$OUT_DIR/include/libavfilter" "$OUT_DIR/include/libswscale"
