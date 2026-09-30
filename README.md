# PlayStation SPU two-operator FM demo

This project uses PSn00bSDK to produce `hello.elf` and `hello.exe` for the
original PlayStation from C and Rust source on Apple Silicon Macs. The demo
uses SPU pitch modulation (PMOD) as a two-operator FM-like synthesizer. Voice 0
plays a selectable waveform on the modulator and voice 1 plays an independently
selectable waveform on the carrier. The modulator is muted
in the stereo mix while its output still changes the carrier's pitch at the
SPU sample rate. Frequency ratios from 1:4 through 5:1 and eleven modulation
depths are selectable. Independent one-shot AR envelopes shape the modulator
and carrier; the modulator envelope changes the brightness over the note.

Both envelopes use the SPU ADSR generators. A 1 kHz hardware timer reads their
levels for the meters and ends playback after the longer envelope. Five shapes
(sine, square, saw, triangle, and repeating noise) are generated as single-cycle
56-sample loops at eleven modulation depths. Their SPU ADPCM encoding runs at
build time, so startup only copies the prepared loops to SPU RAM. Ratios stop
at 5:1 to keep every note below the SPU pitch ceiling. The carrier uses a loop
at fixed amplitude. Depth zero produces an unmodulated carrier waveform.
The encoder searches predictor and shift combinations against decoded error and
carries predictor history across repeated cycles. Source loops have zero mean,
and the encoded loops keep their steady-state mean within one 16-bit PCM unit
of zero. No external audio assets are required.

## C and Rust boundary

C owns the PSn00bSDK entry point, timer callback, SPU/GPU registers, pad reads,
and drawing. Rust provides a `no_std` static library with waveform and ADPCM
generation, settings and button repeat rules, and envelope calculations. The
fixed-layout functions in `src/synth.h` are the only interface between them.
CMake builds `core` for `mipsel-sony-psx` and links the library into the SDK
executable. The conversion wrapper adapts Rust-generated ELF metadata for the SDK
`elf2x` tool.

## Initial setup

Prerequisites are an Apple Silicon Mac, Xcode Command Line Tools, Homebrew,
Git, and [rustup](https://rustup.rs). The setup installs CMake, Ninja, the
MIPS GCC/binutils toolchain from the official PCSX-Redux definitions,
PSn00bSDK, PCSX-Redux, its bundled OpenBIOS, and the pinned Rust nightly with
`rust-src`. It does not upgrade all existing Homebrew packages. The SDK and
emulator are installed inside the project.

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
it. Hold an adjustment button to repeat it, or use L1 and R1 for numeric changes
ten times as large. `MIDI NOTE` ranges from 24 to 72. `MOD RATIO` selects
1:4 through 5:1 in 0.25 steps. `MOD WAVE` and `OUT WAVE`
select sine, square, saw, triangle, or noise independently. `MOD DEPTH` selects
0 through 10; higher depths produce wider pitch deviation. The 9 and 10 steps
approach the largest modulation available from one SPU voice.
Each operator's attack and release uses 1 ms steps up to 500 ms and selects the
closest hardware-supported ADSR rate. Changes take effect on the next trigger.
Press the key mapped to Cross to trigger the one-shot envelopes. To change the
mapping, press Escape and open `Configuration > Controls`. F5 runs the program
and F6 pauses it.
Because `run.sh` disables Dynarec and enables the debugger, `Debug > Show
Assembly` can be used to inspect breakpoints and CPU state.

## Artifacts and directories

- `build/debug/hello.elf`: MIPS ELF containing symbols and DWARF debug data.
- `build/debug/hello.exe`: PS-X EXE loaded directly by PCSX-Redux; it is not a
  Windows executable.
- `build/release/`: Equivalent artifacts for the Release configuration.
- `src/main.c`: PSn00bSDK initialization, SPU/GPU/pad access, timer interrupt,
  and rendering.
- `rust/build.rs`: Encodes the ADPCM loops during the host build.
- `rust/src/lib.rs`: C ABI entry points for the `no_std` Rust library.
- `rust/src/synth.rs`: Settings, button repeat state, and operator ADSR programs.
- `rust/src/waveform.rs`: Waveform generation, depth scaling, and ADPCM loops.
- `rust/src/adpcm.rs`: Stateful SPU ADPCM block encoder.
- `src/synth.h`: C ABI shared by the C and Rust layers.
- `scripts/elf2x-rust.py`: Filters Rust ELF stack metadata for the SDK converter.
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
