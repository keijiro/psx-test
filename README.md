# PlayStation homebrew sample

This project uses PSn00bSDK to produce `hello.elf` and `hello.exe` for the
original PlayStation from C source on Apple Silicon Macs. The sample uses
double buffering to draw text and a moving square. With the default
PCSX-Redux keyboard mapping, the arrow keys also move the square.

## Initial setup

Prerequisites are an Apple Silicon Mac, Xcode Command Line Tools, Homebrew,
and Git. The setup installs CMake, Ninja, the MIPS GCC/binutils toolchain from
the official PCSX-Redux definitions, PSn00bSDK, PCSX-Redux, and its bundled
OpenBIOS. It does not upgrade all existing Homebrew packages, and installs the
SDK and emulator inside the project.

```sh
./scripts/setup.sh
source scripts/env.sh
cmake --preset debug
cmake --build --preset debug
./scripts/run.sh
```

`setup.sh` is safe to rerun. It does not rebuild the toolchain, SDK, or emulator
when the pinned working versions are already installed. It does not modify the
user's shell configuration files.

If PCSX-Redux asks about automatic updates on its first launch, choose either
`Enable auto update` or `No thanks` according to your preference. The official
macOS build is not signed with a developer certificate. If macOS blocks it,
right-click the app in Finder and select `Open`, or allow it explicitly under
`Privacy & Security` in System Settings.

## Daily use

Load the environment first in each new zsh session:

```sh
source scripts/env.sh
```

Debug build:

```sh
cmake --preset debug
cmake --build --preset debug
./scripts/run.sh
```

Release build:

```sh
cmake --preset release
cmake --build --preset release
./scripts/run.sh build/release/hello.exe
```

`run.sh` loads the EXE directly with OpenBIOS, the interpreter CPU, and the
built-in debugger enabled. Set `PCSX_REDUX`, `PCSX_REDUX_BIOS`, or
`PCSX_REDUX_DATA` to use a different emulator binary, BIOS, or personal data
directory, respectively.

The default PCSX-Redux configuration maps the arrow keys to the D-pad. To
change the mapping, press Escape and open `Configuration > Controls`. F5 runs
the program and F6 pauses it. Because `run.sh` disables Dynarec and enables the
debugger, `Debug > Show Assembly` can be used to inspect breakpoints and CPU
state.

## Artifacts and directories

- `build/debug/hello.elf`: MIPS ELF containing symbols and DWARF debug data.
- `build/debug/hello.exe`: PS-X EXE loaded directly by PCSX-Redux; it is not a
  Windows executable.
- `build/release/`: Equivalent artifacts for the Release configuration.
- `src/main.c`: Sample text, double-buffered rendering, animation, and D-pad
  input.
- `scripts/env.sh`: zsh environment configuration for the SDK, emulator, and
  `PATH`.
- `scripts/setup.sh`: Fetches, verifies, builds, and installs pinned
  dependencies.
- `scripts/run.sh`: Launches the emulator with verified CLI options.
- `patches/`: Narrow compatibility patches for building PSn00bSDK v0.24 on
  macOS 26 with GCC 16.
- `.local/`: SDK, emulator, downloads, and personal emulator configuration.
- `third_party/`: Pinned PSn00bSDK source and submodules.
- `build/`: Build artifacts and validation logs.

Pinned versions and checksums are recorded in [toolchain.lock](toolchain.lock),
and completed validation is recorded in
[docs/validation.md](docs/validation.md).
