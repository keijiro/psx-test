# PlayStation SPU wavetable synthesis demo

This project uses PSn00bSDK to produce `hello.elf` and `hello.exe` for the
original PlayStation from C source on Apple Silicon Macs. The demo is a
two-voice wavetable synthesizer. Each voice can use a sine, triangle, saw,
square, or deterministic noise table, and complementary voice volumes linearly
interpolate between the selected pair. A MIDI note number chooses the base
pitch. Independent one-shot AR envelopes control amplitude and waveform mix,
and a curve control shapes a positive or negative pitch sweep.

The amplitude AR runs on the voices' SPU ADSR generators. A 1 kHz hardware
timer updates the complementary mix volumes and pitch registers, and reads the
hardware envelope level for visualization. All five 56-sample tables are
generated and encoded to looping SPU ADPCM at startup. The encoder searches all
predictor and shift combinations against decoded error and carries predictor
history across repeated cycles, so no external audio assets are required.

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

Use Up and Down on the D-pad to select a setting, and Left and Right to adjust
it. `WAVE A` and `WAVE B` select the two source tables. `MIDI NOTE` ranges from
24 to 96. Amplitude and mix attack/release times use 5 ms steps up to 500 ms;
the amplitude controls select the closest hardware-supported ADSR rates.
`PITCH SWEEP` offsets the initial pitch by -24 to +24 semitones and
`PITCH CURVE` sets the exponential falloff from 1 to 8. Press the key mapped to
Cross to trigger the one-shot envelopes. To change the mapping, press Escape
and open `Configuration > Controls`. F5 runs the program and F6 pauses it.
Because `run.sh` disables Dynarec and enables the debugger, `Debug > Show
Assembly` can be used to inspect breakpoints and CPU state.

## Artifacts and directories

- `build/debug/hello.elf`: MIPS ELF containing symbols and DWARF debug data.
- `build/debug/hello.exe`: PS-X EXE loaded directly by PCSX-Redux; it is not a
  Windows executable.
- `build/release/`: Equivalent artifacts for the Release configuration.
- `src/main.c`: Runtime SPU ADPCM wavetable generation, two-voice interpolation,
  button-triggered AR and pitch envelopes, controls, and visualization.
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
