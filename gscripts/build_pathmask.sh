#!/usr/bin/env bash
# Build PathMask against this exact Sultan kernel output, not a generic GKI SDK.
# The .ko and KSU zip are deliberately separate from the AnyKernel boot image.
set -Eeuo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=config.sh
source "${SCRIPT_DIR}/config.sh"

[[ $# -eq 2 ]] || die "Usage: $0 <gs201|zuma|zumapro> <variant>"
TARGET="$1"
VARIANT="$2"
require_target "$TARGET"
require_variant "$VARIANT"

# Pin upstream to an auditable source revision; don't silently pull new hooks.
readonly PATHMASK_REPO='https://github.com/Andrea-lyz/LKM-PathMask.git'
readonly PATHMASK_COMMIT='4adf552ab653fcd6c4512fc8927d8f172fa9536b'
readonly PATHMASK_KMI='android14-6.1'

[[ -f "$OUT_DIR/.config" ]] || die "Kernel .config missing: build the kernel first"
[[ -f "$OUT_DIR/Module.symvers" ]] || die "Kernel Module.symvers missing: a matching module cannot be built"
grep -qx 'CONFIG_MODULES=y' "$OUT_DIR/.config" || die 'CONFIG_MODULES=y is required for PathMask .ko'

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT
PM_DIR="$WORK_DIR/PathMask"

git init -q "$PM_DIR"
git -C "$PM_DIR" remote add origin "$PATHMASK_REPO"
git -C "$PM_DIR" fetch -q --depth=1 origin "$PATHMASK_COMMIT"
git -C "$PM_DIR" checkout -q --detach FETCH_HEAD
[[ "$(git -C "$PM_DIR" rev-parse HEAD)" == "$PATHMASK_COMMIT" ]] || die 'PathMask commit mismatch'

msg "Fetching pinned PathMask dependencies"
sh "$PM_DIR/kernel/scripts/fetch-deps.sh"

msg "Building PathMask against ${TARGET} / ${VARIANT} Sultan kernel"
export ARCH=arm64 SUBARCH=arm64
export CROSS_COMPILE="${TOOLCHAIN_BIN}/aarch64-linux-"
export CC="${TOOLCHAIN_BIN}/aarch64-linux-gcc"
CONFIG_KSU=m make -C "$PM_DIR/kernel" \
    KDIR="$OUT_DIR" VER="$PATHMASK_KMI" -j"$(nproc)"

MODULE_DIR="$PM_DIR/kernel/out/$PATHMASK_KMI"
[[ -s "$MODULE_DIR/pathmask.ko" ]] || die 'PathMask .ko missing after build'
[[ -s "$MODULE_DIR/procguard.ko" ]] || die 'PathMask procguard.ko missing after build'

mkdir -p "$DIST_DIR"
PREFIX="PathMask_${TARGET}_${VARIANT}_${PATHMASK_KMI}"
cp "$MODULE_DIR/pathmask.ko" "$DIST_DIR/${PREFIX}.ko"
cp "$MODULE_DIR/procguard.ko" "$DIST_DIR/${PREFIX}_procguard.ko"

# Never inherit the upstream generic-KMI update channel for a device-specific build.
NO_UPDATE_JSON=1 sh "$PM_DIR/tools/package_ksu.sh" \
    "$MODULE_DIR/pathmask.ko" "$DIST_DIR/${PREFIX}_ksu.zip"

[[ -s "$DIST_DIR/${PREFIX}_ksu.zip" ]] || die 'PathMask KernelSU zip missing'
msg "PathMask artifacts ready: ${DIST_DIR}/${PREFIX}*"
