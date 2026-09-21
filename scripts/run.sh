#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)

EMULATOR=${PCSX_REDUX:-"${PROJECT_ROOT}/.local/PCSX-Redux.app/Contents/MacOS/PCSX-Redux"}
BIOS=${PCSX_REDUX_BIOS:-"${PROJECT_ROOT}/.local/PCSX-Redux.app/Contents/Resources/share/pcsx-redux/resources/openbios.bin"}
DATA_DIR=${PCSX_REDUX_DATA:-"${PROJECT_ROOT}/.local/pcsx-redux-data"}
EXE_PATH=${1:-"${PROJECT_ROOT}/build/debug/hello.exe"}

if [ ! -f "${EXE_PATH}" ] && [ "$#" -eq 0 ] && [ -f "${PROJECT_ROOT}/build/release/hello.exe" ]; then
  EXE_PATH="${PROJECT_ROOT}/build/release/hello.exe"
fi

if [ ! -x "${EMULATOR}" ]; then
  echo "PCSX-Redux is missing: ${EMULATOR}" >&2
  echo "Run ./scripts/setup.sh first." >&2
  exit 1
fi
if [ ! -f "${BIOS}" ]; then
  echo "OpenBIOS is missing: ${BIOS}" >&2
  echo "Run ./scripts/setup.sh first." >&2
  exit 1
fi
if [ ! -f "${EXE_PATH}" ]; then
  echo "PS-X EXE is missing: ${EXE_PATH}" >&2
  echo "Run: source scripts/env.sh && cmake --preset debug && cmake --build --preset debug" >&2
  exit 1
fi

mkdir -p "${DATA_DIR}"

exec "${EMULATOR}" \
  -portable "${DATA_DIR}" \
  -bios "${BIOS}" \
  -exe "${EXE_PATH}" \
  -run \
  -interpreter \
  -debugger

