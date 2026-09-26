# Validation record

## C and Rust PS1 build

On 2026-09-26, waveform generation, ADPCM encoding, settings rules, and
envelope calculations were moved to a `no_std` Rust static library built for
`mipsel-sony-psx`. C retains SDK calls, hardware register access, interrupts,
and rendering. Native Rust unit tests passed (3/3). Clean Debug and Release
builds through the project CMake presets produced MIPS-I ELF files and PS-X
EXEs of about 114 KB and 22 KB respectively.

PSn00bSDK `elf2x` treated Rust's trailing `GNU_STACK` program header as a
load segment at address zero. The build now filters this metadata header in a
temporary ELF copy passed to `elf2x`; the linked ELF is preserved for symbols
and debugging. The linker still reports mixed abicalls/non-abicalls warnings.

The emulator exited with code 255 before logging OpenBIOS startup for both
this build and the pre-change Release EXE in this session. Screen, controls,
and audio behavior therefore remain unverified here.

## General wavetable synthesis

On 2026-09-22, the fixed kick voice was expanded into a two-wave wavetable
synthesizer. Sine, triangle, saw, square, and deterministic noise sources are
encoded as looping SPU ADPCM; MIDI notes 24--96, amplitude and mix AR times,
and pitch sweep amount and curvature are editable from the controller.

Clean Debug and Release builds completed successfully and produced MIPS-I ELF
and PS-X EXE artifacts. The Debug executable remained running under OpenBIOS
in PCSX-Redux without an emulator or SDK error. The automation interface could
not bind to the emulator's unbundled macOS window, so the revised screen and
audio output have not been visually or aurally evaluated, nor checked on
original hardware.

## Predictive ADPCM sine encoding

On 2026-09-21, the sine wavetable's direct four-bit quantization was replaced
with runtime PSX ADPCM encoding. The encoder evaluates every supported
predictor and shift value against the reconstructed samples and carries the
decoder history across repeated encoding passes to stabilize the loop
boundary. The sine source retains the previous peak level while increasing
from 15 amplitude steps to a 16-bit table.

An independent decoder simulation measured approximately 5.57% THD in the old
table and 0.17% in the steady-state encoded loop. Both Debug and Release builds
completed successfully. The revised waveform has not been evaluated by ear or
checked on original hardware.

## SPU ADSR wavetable envelopes

On 2026-09-21, per-millisecond CPU volume writes were replaced with native SPU
ADSR envelopes for the sine and noise voices. Both Debug and Release builds
completed successfully. The Debug executable remained running under OpenBIOS
in PCSX-Redux without an emulator or SDK error. The revised envelopes have not
been evaluated by ear or checked on original hardware.

## Wavetable kick synthesis demo

On 2026-09-21, the melodic sine-to-saw demonstration was replaced with a
120 BPM synthesized kick. The implementation crossfades a short fixed-noise
transient into a sine body, sweeps the shared pitch from 180 Hz to 46 Hz, and
applies a nonlinear amplitude decay. Both Debug and Release builds completed
successfully and produced MIPS-I ELF and PS-X EXE artifacts. The Debug
executable remained running under OpenBIOS in PCSX-Redux without an emulator or
SDK error. The audio output has not been evaluated by ear or checked on
original hardware.

## Wavetable morph demo

On 2026-09-21, the single-channel square-wave test was replaced with the
two-voice sine-to-saw wavetable morph demo. Both Debug and Release builds
completed successfully and produced MIPS-I ELF and PS-X EXE artifacts. The
Debug executable was loaded with OpenBIOS in PCSX-Redux and continued running
without an emulator or SDK error.

The audio output itself has not been checked on original hardware. The
remaining sections record the initial environment validation from 2026-09-16.

Validation date: 2026-09-16 (Asia/Tokyo)

The host was macOS 26.6.2 on arm64, with Xcode 27.0, Apple Clang 21.0.0,
and Homebrew 6.0.22. See `toolchain.lock` for the pinned versions and checksums.

## Setup and toolchain

Running `./scripts/setup.sh` installed CMake 4.4.3, Ninja 1.13.2,
binutils 2.47, GCC 16.2.0, PSn00bSDK v0.24, and PCSX-Redux build 250. A
second run finished in about eight seconds and skipped rebuilding the working
SDK and emulator.

A minimal C function was compiled with these options:

```sh
printf '%s\n' 'int add(int a, int b) { return a + b; }' |
  mipsel-none-elf-gcc -march=r3000 -mabi=32 -EL -x c -c -o build/toolchain-check/probe.o -
mipsel-none-elf-objdump -f build/toolchain-check/probe.o
```

The result was `elf32-littlemips`, `architecture: mips:3000`; `file` identified
it as `ELF 32-bit LSB relocatable, MIPS-I`. The compiler, assembler, linker,
and objdump all ran successfully.

PSn00bSDK v0.24 required three narrow compatibility fixes for Apple Clang 21
and GCC 16:

- Always include `stdlib.h` in the host-side LZP compressor.
- Give the draw-queue function pointer the three-argument type used by the
  implementation.
- Map `stat64` to `stat` in `mkpsxiso` on macOS.

The fixes are stored in `patches/psn00bsdk-v0.24-macos-clang.patch`. No broad
warning or error suppression was used. The SDK's Debug and Release libraries
and host tools, including `elf2x` and `mkpsxiso`, were built and installed in
`.local/psn00bsdk`.

The SDK's `beginner/hello` sample was also built separately and verified as a
MIPS-I ELF with a `PS-X EXE` signature.

## Project build

The documented procedure was run from a new zsh session:

```sh
source scripts/env.sh
cmake --preset debug
cmake --build --preset debug
cmake --preset release
cmake --build --preset release
```

Results:

| Artifact | Identification | SHA-256 |
| --- | --- | --- |
| `build/debug/hello.elf` | MIPS-I LSB ELF with debug data and symbols | `25bdfa29b042fef7da69afd0cd85a1af56b8e3d9a3a2afb8da5461a8de048c2e` |
| `build/debug/hello.exe` | Sony PlayStation executable, `PS-X EXE` | `2f7a01440be49b3f89f7a44d2b570b48ca0b74511fc6c6470e1945b573775904` |
| `build/release/hello.elf` | MIPS-I LSB ELF | `f54c7d1492d8e12d68f603dd80af396d336e90058ce243bb31fa3b803158d0da` |
| `build/release/hello.exe` | Sony PlayStation executable, `PS-X EXE` | `8c36aa4934675c5012f9e83837cf4c6edb5279989b7e5125e7e83d571e71f1eb` |

The final source was configured and built in a new
`build/final-clean-20260916` directory, producing the same PS-X EXE without
relying on existing intermediate artifacts.

## Emulator and display

The official AppDistrib macOS ARM build 250 was used. PCSX-Redux recognized
the bundled `openbios.bin` as `OpenBIOS detected (0b0359a7)` and loaded
`hello.exe` directly. The validation log is at
`build/validation/pcsx-debug.log`.

The following behavior was visually verified:

- `PSN00BSDK VALIDATED` and `D-PAD / ARROW KEYS: MOVE` appeared on a dark
  navy background.
- The yellow square moved horizontally each frame and changed color over time.
  The measured rate was about 59.9 FPS while the menu was visible.
- Twenty Up Arrow inputs moved the top of the square about 125 screen pixels
  upward.
- After changing the background color and first line of text, rebuilding and
  restarting showed the new dark navy color and `VALIDATED` text.

The configuration saved by PCSX-Redux was also checked for `Dynarec: false`,
`Debug.Debug: true`, and the arrow-key SDL scancodes: Left 80, Right 79, Up 82,
and Down 81.

## Debugger

The Debug ELF symbols identified `main = 0x80010474`. PCSX-Redux was launched
with `-interpreter -debugger`, and an execution breakpoint was set at that
address through the Lua API.

The log recorded:

```text
Breakpoint triggered: PC=0x80010474 - Cause: 80010474::Exec::4 (Lua Breakpoint)
```

In the Assembly view, the yellow PC arrow pointed to `RAM:80010474`, where the
instruction word was `27bdffd8` (`addiu sp,sp,-40`). After execution resumed
with F5, the PC arrow disappeared and GPU and controller initialization
continued in the log.
