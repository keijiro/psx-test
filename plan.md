# Plan for an original PlayStation homebrew development environment

## Objective

Build a development environment in `/Users/keijiro/Projects/psx` that can
produce executables for the original PlayStation from C source, then run and
validate them in an emulator on macOS.

This document is an implementation plan for AI agents. Planning and executing
the environment setup are separate tasks; an agent asked to perform the setup
should complete the stages below in order.

## Assumptions and selected components

Observations made when this plan was written:

- The working directory was empty, with no existing Git repository.
- The host was Apple Silicon (arm64) running macOS 26.6.2.
- Homebrew, Clang, Git, and make were available on `PATH`.
- CMake, Ninja, a MIPS GCC toolchain, Docker, and an emulator CLI were not found
  on `PATH`. The presence of GUI applications was not checked.
- Building the SDK and running the emulator had not been validated.

Recheck the environment before starting implementation. Do not treat these
observations as the current state or as a guarantee that anything works.

| Purpose | Selected candidate | Policy |
| --- | --- | --- |
| Application language | C | Use C for the initial sample and acceptance criteria |
| Cross compiler | GCC / binutils, `mipsel-none-elf` | Use builds that run on macOS arm64 |
| SDK | PSn00bSDK | Use its official CMake definitions and templates |
| Build system | CMake + Ninja | Provide Debug and Release presets |
| Emulator | PCSX-Redux Mac ARM build | Validate execution with the built-in debugger |
| BIOS | OpenBIOS | Use it for initial validation |
| CD image generation | SDK-provided `mkpsxiso` | Leave as an optional post-completion step |

Basic processing flow:

```text
C source + PSn00bSDK
        | CMake / Ninja / MIPS GCC
        v
hello.elf (retains debug information)
        | SDK executable conversion
        v
hello.exe (PS-X EXE format)
        | PCSX-Redux + OpenBIOS
        v
Rendering, input, and debugging validation
```

`hello.exe` is a PS-X EXE for PlayStation, not a Windows executable.

## Scope

The required scope covers the toolchain, SDK, emulator, C sample, build and run
scripts, and the records needed to reproduce the environment.

The following are not required for initial completion:

- A C++ sample or standard-library compatibility checks.
- CD-ROM, audio, memory-card, or 3D-rendering implementation.
- Transfer to or validation on real hardware.
- VS Code or external GDB integration, or CI setup.
- Additional validation with DuckStation.

Do not include downloads of Sony's official SDK, PsyQ, or a real console BIOS
in the setup procedure. If OpenBIOS causes a problem, investigate the cause; do
not automatically download a different BIOS as a workaround.

## Agent workflow

- Read the applicable `AGENTS.md` files and latest user instructions, and
  preserve existing files and work in progress.
- Respect dependencies between stages and satisfy each stage's completion
  criteria before moving to the next.
- Make routine implementation decisions autonomously. Do not ask the user for
  facts that can be determined from the environment.
- Update this document's checklist during setup, recording failures, selected
  versions, and validation results.
- Do not describe untested procedures or speculative commands as completed or
  verified.
- When installation or launch requires approval from the OS or a tool, use its
  designated mechanism. Do not disable security protections globally.
- If the environment prevents a required operation, finish independent work
  and report the incomplete items and required actions precisely.
- Before making a major configuration change, such as replacing the environment
  or introducing a new Docker runtime or VM, explain its reason and impact.

## Planned file layout

```text
psx/
├── plan.md
├── README.md
├── .gitignore
├── CMakeLists.txt
├── CMakePresets.json
├── toolchain.lock
├── src/
│   └── main.c
├── scripts/
│   ├── setup.sh
│   ├── env.sh
│   └── run.sh
├── docs/
│   └── validation.md
├── third_party/          # Downloaded SDK and related sources
├── .local/               # SDK, tools, and emulator installations
└── build/                # Build artifacts and required logs
```

Do not create empty directories or unnecessary configuration files in advance.
Add `third_party/`, `.local/`, `build/`, and personal configuration to
`.gitignore`. Initialize Git when needed; commits, remote setup, and publication
are not required parts of this plan.

## Stage 0: Inspect the environment and dependencies

- [x] Check the OS, CPU, free space, Xcode Command Line Tools, and Homebrew.
- [x] Check the versions and availability of CMake, Ninja, GCC/binutils, and any
  existing SDK.
- [x] Check for an existing PCSX-Redux app and executable.
- [x] Review official documentation and distribution sources, then select SDK,
  compiler, and emulator candidates.

Determine the exact host dependency list from the selected macOS toolchain
definition. Do not indiscriminately upgrade existing packages.

Completion criterion: required additions, usable existing components, and
installation locations are known.

## Stage 1: Install the MIPS toolchain

- [x] Install CMake, Ninja, and required host dependencies.
- [x] Inspect the PCSX-Redux macOS GCC/binutils build definitions.
- [x] Install a `mipsel-none-elf` toolchain for macOS arm64.
- [x] Verify that the compiler, assembler, linker, and binary inspection tools
  start successfully.
- [x] Compile a minimal C file and verify that it is a little-endian MIPS object.

Prefer installation under the project's `.local/` directory. If using the
Homebrew build definitions directly is more appropriate, install under
Homebrew management and record the path and acquisition method.

Do not choose the latest GCC solely because of its version number. Validate its
combination with the SDK and pin a combination that works. Do not substitute
host Clang or a Linux `mipsel-linux-gnu` toolchain for a PlayStation toolchain.

Completion criterion: C code can be compiled into an object for the target CPU,
and the toolchain source and version are recorded.

## Stage 2: Build and smoke-test PSn00bSDK

- [x] Fetch PSn00bSDK at a specific tag or commit, including required
  submodules.
- [x] Build the SDK and host tools with the selected toolchain.
- [x] Install the SDK under `.local/psn00bsdk/`.
- [x] Provide `scripts/env.sh` to set `PSN00BSDK_LIBS` and the required `PATH`.
- [x] Build an SDK-provided minimal template or rendering sample.
- [x] Confirm production of ELF and PS-X EXE files.

Leave compiler flags, the linker script, and executable conversion to the SDK's
CMake definitions. Do not confuse conversion tools that run on the host with
code generated for the PlayStation.

The SDK's official documentation describes macOS validation as limited. If the
build needs a fix, isolate the cause and retain the patch and application
instructions in the project. Do not make it pass by broadly suppressing
warnings or errors.

Completion criterion: the bundled sample produces `*.elf` and `*.exe`, and the
EXE begins with the `PS-X EXE` signature.

## Stage 3: Implement the C application and development workflow

- [x] Create a root CMake project based on the official template.
- [x] Provide Debug and Release configure/build presets.
- [x] Implement text, an animated shape, and directional input in `src/main.c`.
- [x] Follow the SDK's recommended drawing-buffer update and VBlank sync
  procedure.
- [x] Produce `hello.elf` and `hello.exe`.
- [x] Implement the finalized setup procedure in `scripts/setup.sh`.

Keep the first sample to simple 2D graphics with no external assets. Make it
possible to validate input through the emulator's keyboard mapping.

Script requirements:

- Resolve the project root from each script's location, independent of the
  working directory.
- Make `env.sh` sourceable from zsh without changing the user's shell
  configuration files.
- Quote paths correctly and constrain artifact, download, and installation
  destinations.
- Make `setup.sh` rerunnable without rebuilding a valid existing installation
  every time.
- Detect fetch and build failures, and do not treat incomplete installations as
  complete.
- Record source commits, distribution identifiers, and available checksums in
  `toolchain.lock`.
- Use pinned dependencies for reconstruction; make upgrades explicit.

Provide this workflow after setup:

```sh
./scripts/setup.sh
source scripts/env.sh
cmake --preset debug
cmake --build --preset debug
./scripts/run.sh
```

This is the interface to implement, not a set of commands known to work when
the plan was written.

Completion criterion: the custom C sample builds in both Debug and Release, and
environment setup and reconstruction are scripted.

## Stage 4: Install the emulator and provide the run workflow

- [x] Fetch the PCSX-Redux Mac ARM distribution from its official source, or
  reuse a suitable existing installation.
- [x] Record identifying information for the selected distribution.
- [x] Check whether OpenBIOS is bundled and configure an available copy. If
  necessary, obtain or build it using the official procedure.
- [x] Launch the emulator and verify BIOS and graphics initialization.
- [x] Load and run `hello.exe` directly.
- [x] Create `scripts/run.sh` using validated CLI options.
- [x] Map the controller D-pad and buttons to the keyboard.

Verify CLI options and the executable location inside the macOS application
against the selected version's help, official documentation, and actual files.
Do not record a guessed launch command as the definitive procedure.

`run.sh` must report a clear error and show the build command when no executable
exists. Allow the emulator path to be overridden through an environment
variable or personal configuration.

Completion criterion: the generated EXE can be launched in PCSX-Redux from the
documented project workflow.

## Stage 5: Validate execution, debugging, and reconstruction

- [x] Text and the shape appear at the intended positions and colors.
- [x] The shape updates for several seconds without freezes or rendering
  corruption.
- [x] Arrow-key input changes the sample state.
- [x] CPU debugging is enabled and Dynarec is disabled when needed.
- [x] A breakpoint at a known function or instruction address stops execution,
  and execution can resume.
- [x] Registers or memory are inspected while stopped, using ELF symbols or
  disassembly to identify the address.
- [x] A text or color change appears after rebuilding and restarting.
- [x] The final source builds in a separate empty build directory without
  relying on existing intermediate artifacts.
- [x] A new shell can proceed from `env.sh` through build and execution.

When GUI interaction and screenshots are available, use them to validate actual
rendering and input. A running process alone is not sufficient validation. If
the environment cannot operate or observe the GUI, state that limitation and
the user-side checks required, and leave the relevant checkbox incomplete.

Completion criterion: rendering, input, debugging, change propagation, and a
clean build are verified.

## Stage 6: Complete handoff documentation

- [x] Document initial setup, routine build/run steps, and emulator settings in
  `README.md`.
- [x] Explain each artifact and the purpose of major directories in `README.md`.
- [x] Record the selected SDK, GCC, binutils, emulator, and OpenBIOS versions in
  `toolchain.lock`, retaining identifying information for bundled components.
- [x] Record host dependencies and the validated CMake and Ninja versions.
- [x] Summarize commands, results, emulator settings, and required logs or
  screenshots in `docs/validation.md`.
- [x] Update this checklist to reflect actual progress.

The completion report must include the implementation, routine commands,
validation results, and remaining limitations. Do not report the environment
setup as complete while any required stage remains incomplete.

## Troubleshooting

| Problem | Investigation and response |
| --- | --- |
| GCC/binutils does not build on macOS | Compare the selected version and host dependencies with the official macOS build definitions; record any switch to a compatible version |
| SDK CMake configuration fails | Check supported CMake versions, the toolchain `PATH`, `PSN00BSDK_LIBS`, and submodule state |
| Host tools and target code are mixed | Inspect the CMake configuration, compiler, and artifact architecture |
| The EXE does not start or shows a black screen | Test the bundled sample, then isolate the format, loading procedure, BIOS initialization, and rendering in that order |
| OpenBIOS causes a problem | Compare against SDK samples and inspect logs and the debugger; report the dependency if a real-hardware BIOS proves necessary |
| The debugger does not stop | Check debugger enablement, Dynarec configuration, the breakpoint address, and the running target |
| Native build problems remain unresolved | Propose moving the build to a Linux container while retaining execution on macOS; if no container runtime exists, also explain the installation impact |

Even when using a Linux container, compare the host CPU with the distributed
toolchain's CPU. Do not assume an x86_64 Linux distribution runs directly on
Apple Silicon.

## Optional follow-up stages

Perform these only on request after the required stages are complete:

1. CD boot: add `SYSTEM.CNF`, a disc-layout XML file, and external assets;
   produce BIN/CUE files with `mkpsxiso`; separately validate disc boot and
   asset loading.
2. Alternative emulator: validate with DuckStation using a suitable BIOS
   supplied by the user; do not require it in the initial configuration.
3. Source-level debugging: connect a MIPS-capable GDB to the PCSX-Redux GDB
   server and use ELF symbols to stop in C source.
4. Automation: add smoke tests or CI using emulator control features only when
   needed.

## References

At execution time, verify that each source applies to the selected version.

- [PSn00bSDK](https://github.com/Lameguy64/PSn00bSDK)
- [PSn00bSDK installation](https://github.com/Lameguy64/PSn00bSDK/blob/master/doc/installation.md)
- [PSn00bSDK toolchain setup](https://github.com/Lameguy64/PSn00bSDK/blob/master/doc/toolchain.md)
- [PSn00bSDK CMake configuration](https://github.com/Lameguy64/PSn00bSDK/blob/master/doc/cmake_reference.md)
- [PSn00bSDK official template](https://github.com/Lameguy64/PSn00bSDK/tree/master/template)
- [PCSX-Redux and macOS setup](https://github.com/grumpycoders/pcsx-redux)
- [PCSX-Redux macOS toolchain definitions](https://github.com/grumpycoders/pcsx-redux/tree/main/tools/macos-mips)
- [OpenBIOS](https://github.com/grumpycoders/pcsx-redux/tree/main/src/mips/openbios)
- [PCSX-Redux debugging](https://pcsx-redux.consoledev.net/Debugging/introduction/)
- [PCSX-Redux GDB server](https://pcsx-redux.consoledev.net/Debugging/gdb-server/)
