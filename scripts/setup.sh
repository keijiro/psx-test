#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)

SDK_TAG="v0.24"
SDK_COMMIT="06e65bea3a778b2dae5af77a7935ae3868ddd4d3"
PCSX_COMMIT="c2e2dec197d3eb8f3db2ee63b8037321dbe1085e"
PCSX_BUILD_ID="250"
BINUTILS_VERSION="2.47"
GCC_VERSION="16.2.0"

FORMULA_BASE="https://raw.githubusercontent.com/grumpycoders/pcsx-redux/${PCSX_COMMIT}/tools/macos-mips"
BINUTILS_FORMULA_SHA256="20c8043f71773402683d30e0a611586bf6755f721421a4b435f09b050c955f03"
GCC_FORMULA_SHA256="d6a9737b2bd04031ef7a4dfa8e163338feb214837fa466f0d37bed03166a3936"
PCSX_DMG_URL="https://distrib.app/storage/assets/df7/d54/7b1/7fb5a8875b24e0329d2e496aed2e88d0b800935f5df8ac3172c0578/PCSX-Redux-c2e2dec1-Arm.dmg"
PCSX_DMG_SHA256="8022e3b3158fc2aa6f742b83e582783552fc2adebf9af6cd94da22786703d882"

SDK_SOURCE="${PROJECT_ROOT}/third_party/PSn00bSDK"
SDK_INSTALL="${PROJECT_ROOT}/.local/psn00bsdk"
SDK_BUILD="${PROJECT_ROOT}/build/psn00bsdk"
PATCH_FILE="${PROJECT_ROOT}/patches/psn00bsdk-v0.24-macos-clang.patch"
FORMULA_DIR="${PROJECT_ROOT}/.local/toolchain-formulas"
DOWNLOAD_DIR="${PROJECT_ROOT}/.local/downloads"
PCSX_APP="${PROJECT_ROOT}/.local/PCSX-Redux.app"
PCSX_DMG="${DOWNLOAD_DIR}/PCSX-Redux-c2e2dec1-Arm.dmg"

die() {
  echo "setup: $*" >&2
  exit 1
}

sha256_of() {
  shasum -a 256 "$1" | awk '{ print $1 }'
}

fetch_checked() {
  local url=$1
  local destination=$2
  local expected=$3
  local temporary="${destination}.part"

  if [ -f "${destination}" ] && [ "$(sha256_of "${destination}")" = "${expected}" ]; then
    return
  fi

  rm -f "${temporary}"
  curl -fL --retry 3 --output "${temporary}" "${url}"
  [ "$(sha256_of "${temporary}")" = "${expected}" ] || die "checksum mismatch: ${url}"
  mv "${temporary}" "${destination}"
}

[ "$(uname -s)" = "Darwin" ] || die "this setup currently supports macOS only"
[ "$(uname -m)" = "arm64" ] || die "this setup is pinned to Apple Silicon (arm64)"
command -v brew >/dev/null || die "Homebrew is required"
command -v git >/dev/null || die "Git is required"
export PATH="${HOME}/.cargo/bin:${PATH}"
command -v rustup >/dev/null || die "rustup is required; install it from https://rustup.rs"

rustup toolchain install nightly-2026-09-26 --profile minimal --component rust-src

mkdir -p "${FORMULA_DIR}" "${DOWNLOAD_DIR}" "${PROJECT_ROOT}/third_party" "${PROJECT_ROOT}/build"

brew tap nikitabobko/tap

HOST_FORMULAE=(cmake ninja texinfo libmpc mpfr gnu-sed nikitabobko/tap/brew-install-path)
MISSING_FORMULAE=()
for formula in "${HOST_FORMULAE[@]}"; do
  if ! brew list --versions "${formula}" >/dev/null 2>&1; then
    MISSING_FORMULAE+=("${formula}")
  fi
done
if [ "${#MISSING_FORMULAE[@]}" -gt 0 ]; then
  brew install "${MISSING_FORMULAE[@]}"
fi

BINUTILS_FORMULA="${FORMULA_DIR}/mipsel-none-elf-binutils.rb"
GCC_FORMULA="${FORMULA_DIR}/mipsel-none-elf-gcc.rb"
fetch_checked "${FORMULA_BASE}/mipsel-none-elf-binutils.rb" "${BINUTILS_FORMULA}" "${BINUTILS_FORMULA_SHA256}"
fetch_checked "${FORMULA_BASE}/mipsel-none-elf-gcc.rb" "${GCC_FORMULA}" "${GCC_FORMULA_SHA256}"

if ! brew list --versions mipsel-none-elf-binutils 2>/dev/null | grep -q " ${BINUTILS_VERSION}$"; then
  brew install-path "${BINUTILS_FORMULA}"
fi
if ! brew list --versions mipsel-none-elf-gcc 2>/dev/null | grep -q " ${GCC_VERSION}$"; then
  brew install-path "${GCC_FORMULA}"
fi

command -v mipsel-none-elf-gcc >/dev/null || die "mipsel-none-elf-gcc is unavailable after installation"
[ "$(mipsel-none-elf-gcc -dumpfullversion)" = "${GCC_VERSION}" ] || die "unexpected mipsel-none-elf-gcc version"

if [ ! -d "${SDK_SOURCE}/.git" ]; then
  git clone --branch "${SDK_TAG}" --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/Lameguy64/PSn00bSDK.git "${SDK_SOURCE}"
fi

[ "$(git -C "${SDK_SOURCE}" rev-parse HEAD)" = "${SDK_COMMIT}" ] || \
  die "${SDK_SOURCE} exists at a different revision"
git -C "${SDK_SOURCE}" submodule update --init --recursive

if sed -n '1,9p' "${SDK_SOURCE}/libpsn00b/lzp/compress.c" | grep -q '#ifdef LZP_USE_MALLOC'; then
  patch -d "${SDK_SOURCE}" -p1 < "${PATCH_FILE}"
fi

grep -q 'void (\*func)(uint32_t, uint32_t, uint32_t)' "${SDK_SOURCE}/libpsn00b/include/psxgpu.h" || \
  die "PSn00bSDK compatibility patch is incomplete"
grep -q '#define stat64 stat' "${SDK_SOURCE}/tools/mkpsxiso/src/shared/platform.h" || \
  die "mkpsxiso compatibility patch is incomplete"

SDK_MARKER="${SDK_INSTALL}/.installed-${SDK_COMMIT}-${GCC_VERSION}"
if [ ! -f "${SDK_MARKER}" ]; then
  cmake -S "${SDK_SOURCE}" -B "${SDK_BUILD}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${SDK_INSTALL}" \
    -DPSN00BSDK_GIT_TAG="${SDK_TAG}" \
    -DPSN00BSDK_GIT_COMMIT="${SDK_COMMIT}"
  cmake --build "${SDK_BUILD}"
  cmake --install "${SDK_BUILD}"
  touch "${SDK_MARKER}"
fi

[ -x "${SDK_INSTALL}/bin/elf2x" ] || die "PSn00bSDK host tools are missing"
[ -f "${SDK_INSTALL}/lib/libpsn00b/cmake/sdk.cmake" ] || die "PSn00bSDK libraries are missing"

fetch_checked "${PCSX_DMG_URL}" "${PCSX_DMG}" "${PCSX_DMG_SHA256}"

PCSX_VERSION_JSON="${PCSX_APP}/Contents/Resources/share/pcsx-redux/resources/version.json"
if [ -d "${PCSX_APP}" ] && ! grep -q "${PCSX_COMMIT}" "${PCSX_VERSION_JSON}" 2>/dev/null; then
  die "${PCSX_APP} exists but is not the pinned build; move it aside before rerunning setup"
fi

if [ ! -x "${PCSX_APP}/Contents/MacOS/PCSX-Redux" ]; then
  MOUNT_DIR=$(mktemp -d "${TMPDIR:-/tmp}/pcsx-redux-mount.XXXXXX")
  cleanup_mount() {
    hdiutil detach "${MOUNT_DIR}" >/dev/null 2>&1 || true
    rmdir "${MOUNT_DIR}" >/dev/null 2>&1 || true
  }
  trap cleanup_mount EXIT
  hdiutil attach "${PCSX_DMG}" -nobrowse -readonly -mountpoint "${MOUNT_DIR}" >/dev/null
  ditto "${MOUNT_DIR}/PCSX-Redux.app" "${PCSX_APP}"
  cleanup_mount
  trap - EXIT
fi

BIOS="${PCSX_APP}/Contents/Resources/share/pcsx-redux/resources/openbios.bin"
[ -f "${BIOS}" ] || die "the pinned PCSX-Redux build does not contain OpenBIOS"
mkdir -p "${PROJECT_ROOT}/.local/pcsx-redux-data"
"${PCSX_APP}/Contents/MacOS/PCSX-Redux" \
  -portable "${PROJECT_ROOT}/.local/pcsx-redux-data" -dumpproto >/dev/null

echo "Setup complete."
echo "Next: source scripts/env.sh && cmake --preset debug && cmake --build --preset debug"
