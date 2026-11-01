#!/bin/bash
# Runs inside the Miyoo Mini toolchain image; tools/cores/build.sh starts it.
#
#   build-in-container.sh         build, strip and install both cores, then verify
#   build-in-container.sh verify  only re-run the checks on the installed cores
#
# build.sh passes the pinned repositories/commits in MGBA_* and GAMBATTE_*, and
# the image name in TOOLCHAIN.

set -euo pipefail

: "${CROSS_COMPILE:?run inside the toolchain image: source /root/.bashrc first}"

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CORES_DIR="$ROOT/build/cores"
SRC_DIR="$CORES_DIR/src"
BUILD_DIR="$CORES_DIR/build"
JOBS=${JOBS:-8}

# Cortex-A7: ARMv7-A with NEON/VFPv4, hard float, matching the device build.
DEVICE_FLAGS="-marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard"
# Libraries a minimal glibc 2.28 armhf system provides (libpthread/libdl are in libc).
ALLOWED_LIBS="libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 libz.so.1 libstdc++.so.6 libgcc_s.so.1 ld-linux-armhf.so.3"
MAX_GLIBC=2.28
EXPORTS="retro_init retro_run retro_load_game retro_serialize retro_get_memory_data"

build_mgba() {
	echo "building mgba with cmake (-j$JOBS)"
	# mGBA upstream builds the libretro core only through CMake; -O3 and
	# DISABLE_THREADING for the core are set by its own CMakeLists.
	cmake -S "$SRC_DIR/mgba" -B "$BUILD_DIR/mgba" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
		-DCMAKE_C_COMPILER="${CROSS_COMPILE}gcc" \
		-DCMAKE_CXX_COMPILER="${CROSS_COMPILE}g++" \
		-DCMAKE_AR="${CROSS_COMPILE}ar" \
		-DCMAKE_RANLIB="${CROSS_COMPILE}ranlib" \
		-DCMAKE_C_FLAGS="$DEVICE_FLAGS" \
		-DCMAKE_CXX_FLAGS="$DEVICE_FLAGS" \
		-DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
		-DBUILD_LIBRETRO=ON -DSKIP_LIBRARY=ON \
		-DBUILD_GL=OFF -DBUILD_GLES2=OFF -DBUILD_GLES3=OFF \
		-DUSE_ZLIB=OFF -DUSE_PNG=OFF -DUSE_SQLITE3=OFF \
		-DUSE_FFMPEG=OFF -DUSE_ELF=OFF -DUSE_LUA=OFF
	cmake --build "$BUILD_DIR/mgba" --target mgba_libretro -j "$JOBS"
	install_core "$BUILD_DIR/mgba/mgba_libretro.so" mgba LICENSE
}

build_gambatte() {
	echo "building gambatte with its Makefile.libretro (-j$JOBS)"
	# HAVE_NETWORK=0 drops the socket-based link-cable transport; libstdc++ and
	# libgcc are linked statically so the device needs only libc/libm.
	CFLAGS="$DEVICE_FLAGS -fPIC" \
	CXXFLAGS="$DEVICE_FLAGS -fPIC" \
	LDFLAGS="$DEVICE_FLAGS -static-libstdc++ -static-libgcc" \
		make -s -C "$SRC_DIR/gambatte" -f Makefile.libretro platform=unix HAVE_NETWORK=0 \
			CC="${CROSS_COMPILE}gcc" CXX="${CROSS_COMPILE}g++" AR="${CROSS_COMPILE}ar" \
			-j "$JOBS"
	install_core "$SRC_DIR/gambatte/gambatte_libretro.so" gambatte COPYING
}

# install_core <built .so> <core name> <license file in the source tree>
install_core() {
	local built=$1 name=$2 license=$3
	"${CROSS_COMPILE}strip" --strip-unneeded "$built" -o "$CORES_DIR/${name}_libretro.so"
	cp "$SRC_DIR/$name/$license" "$CORES_DIR/${name}_libretro.LICENSE"
	echo "installed $CORES_DIR/${name}_libretro.so"
}

write_versions() {
	: "${MGBA_COMMIT:?tools/cores/build.sh passes MGBA_REPO/MGBA_COMMIT/GAMBATTE_*}"
	{
		echo "# Cores built for the Miyoo Mini (ARMv7 Cortex-A7, hard float, glibc 2.28)."
		echo "# core	repo	commit	license"
		printf 'mgba\t%s\t%s\t%s\n' "$MGBA_REPO" "$MGBA_COMMIT" "MPL-2.0 (mgba_libretro.LICENSE)"
		printf 'gambatte\t%s\t%s\t%s\n' "$GAMBATTE_REPO" "$GAMBATTE_COMMIT" "GPL-2.0 (gambatte_libretro.LICENSE)"
		echo "# toolchain	${TOOLCHAIN:-unknown}"
		echo "# cflags	$DEVICE_FLAGS -O2"
	} > "$CORES_DIR/VERSIONS"
}

FAILED=0
fail() {
	echo "  FAIL: $*" >&2
	FAILED=1
}

verify_core() {
	local name=$1
	local so="$CORES_DIR/${name}_libretro.so" lib
	echo "$name:"
	[ -f "$so" ] || { fail "$name: missing"; return; }
	echo "  file:   $(file -b "$so")"
	echo "  size:   $(wc -c < "$so") bytes stripped"
	case $(file -b "$so") in
	*"ELF 32-bit"*ARM*EABI5*) ;;
	*) fail "$name: not a 32-bit ARM EABI5 ELF" ;;
	esac
	"${CROSS_COMPILE}readelf" -h "$so" | grep -q "hard-float ABI" ||
		fail "$name: not marked hard-float"
	"${CROSS_COMPILE}readelf" -A "$so" | grep -q "Tag_ABI_VFP_args: VFP registers" ||
		fail "$name: does not pass floats in VFP registers"

	for lib in $("${CROSS_COMPILE}readelf" -d "$so" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'); do
		case " $ALLOWED_LIBS " in
		*" $lib "*) echo "  needs:  $lib" ;;
		*) fail "$name: needs $lib, which is not on a minimal glibc system" ;;
		esac
	done

	local max_glibc
	max_glibc=$("${CROSS_COMPILE}readelf" -V "$so" | grep -o 'GLIBC_[0-9.]*' |
		sed 's/GLIBC_//' | sort -Vu | tail -1)
	if [ -z "$max_glibc" ]; then
		fail "$name: no GLIBC symbol versions"
	elif [ "$(printf '%s\n%s\n' "$max_glibc" "$MAX_GLIBC" | sort -V | tail -1)" != "$MAX_GLIBC" ]; then
		fail "$name: needs GLIBC $max_glibc, newer than $MAX_GLIBC"
	else
		echo "  glibc:  $max_glibc (max)"
	fi

	for sym in $EXPORTS; do
		"${CROSS_COMPILE}nm" -D --defined-only "$so" | grep -q " T $sym\$" ||
			fail "$name: $sym is not exported"
	done
	echo "  exports: $EXPORTS"
	[ -f "$CORES_DIR/${name}_libretro.LICENSE" ] ||
		fail "$name: license file missing"
}

verify_all() {
	for name in mgba gambatte; do
		verify_core "$name"
	done
	[ -f "$CORES_DIR/VERSIONS" ] || fail "VERSIONS missing"
	[ "$FAILED" = 0 ] || { echo "verification failed" >&2; exit 1; }
	echo "all checks passed"
}

case ${1:-build} in
build)
	mkdir -p "$BUILD_DIR"
	build_mgba
	build_gambatte
	write_versions
	verify_all
	;;
verify)
	verify_all
	;;
*)
	echo "usage: $0 [build|verify]" >&2
	exit 2
	;;
esac
