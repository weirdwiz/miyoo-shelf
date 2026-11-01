#!/bin/bash
# Cross-compile the two libretro cores Shelf loads on the device.
#
#   tools/cores/build.sh            (or: make cores)
#
# Both cores are cloned at the pinned commits below into build/cores/src and built
# inside the Miyoo Mini toolchain image (amd64, emulated on Apple Silicon). The
# cores are stripped into build/cores/ together with their license files, and
# tools/cores/build-in-container.sh then checks them: armhf ELF with the
# Cortex-A7/VFP flags, no NEEDED library that a minimal glibc 2.28 system lacks,
# the retro_* entry points exported, every GLIBC symbol version <= 2.28.
#
# Re-running reuses an existing checkout of the pinned commit; delete build/cores
# for a from-scratch build.

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CORES_DIR="$REPO_ROOT/build/cores"
SRC_DIR="$CORES_DIR/src"
CONTAINER=${CONTAINER:-$(command -v podman || command -v docker)}
TOOLCHAIN=${TOOLCHAIN:-docker.io/aemiii91/miyoomini-toolchain:latest}

# Pinned upstream commits (default-branch HEADs at the time this was written).
# Exported so the container run below records them in build/cores/VERSIONS.
export MGBA_REPO=https://github.com/libretro/mgba
export MGBA_COMMIT=7a12d6d4b9acb14c0ae62c9166b6a2f3d08007f6
export GAMBATTE_REPO=https://github.com/libretro/gambatte-libretro
export GAMBATTE_COMMIT=77f2567d718b8973d02caa2daa8dce4953faf811

# checkout <name> <repo> <commit>: a shallow tree containing exactly that commit.
checkout() {
	local name=$1 repo=$2 commit=$3
	local dir="$SRC_DIR/$name"
	if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ]; then
		echo "using $name $commit (already checked out)"
		return
	fi
	rm -rf "$dir"
	mkdir -p "$dir"
	git -C "$dir" init -q
	git -C "$dir" remote add origin "$repo"
	git -C "$dir" fetch -q --depth 1 origin "$commit"
	git -C "$dir" checkout -q FETCH_HEAD
	echo "fetched $name $commit"
}

mkdir -p "$CORES_DIR"
if [ -z "$CONTAINER" ]; then
	echo "podman or docker is required to build the cores" >&2
	exit 1
fi
checkout mgba "$MGBA_REPO" "$MGBA_COMMIT"
checkout gambatte "$GAMBATTE_REPO" "$GAMBATTE_COMMIT"

echo "building inside $TOOLCHAIN"
started=$(date +%s)
"$CONTAINER" run --rm --platform linux/amd64 \
	-v "$REPO_ROOT":/root/workspace -w /root/workspace \
	-e MGBA_REPO -e MGBA_COMMIT -e GAMBATTE_REPO -e GAMBATTE_COMMIT \
	-e TOOLCHAIN="$TOOLCHAIN" \
	"$TOOLCHAIN" /bin/bash -c "source /root/.bashrc && tools/cores/build-in-container.sh"
echo "done in $(( $(date +%s) - started ))s: $CORES_DIR/mgba_libretro.so $CORES_DIR/gambatte_libretro.so"
