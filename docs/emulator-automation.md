# Emulator control over HTTP and Lua

This workflow is adapted from `psx-grid/docs/emulator-automation.md` and its
HTTP/Lua bridge. It controls the ordinary synth executable when computer-use
cannot bind the native PCSX-Redux window. The copied knowledge and supporting
scripts are self-contained in this repository.

## Control and observation

The runner sends bounded HTTP commands, injects buttons through Redux's SIO
API, waits for GPU vsync events, pauses, and captures the emulated display.
The synth still uses its normal pad driver, settings, renderer, timer, and SPU
code. RAM is only read to check settings, prepared and triggered programs,
and the envelope levels sampled by the synth.

| Capability | Route | Scope |
| --- | --- | --- |
| Launch without native menus | CLI `-dofile`, `-webserver`, `-no-ui` | Pinned build 250 with bundled OpenBIOS |
| Pause, resume, query execution | `/api/v1/execution-flow` | Lua status stays responsive while paused |
| Button presses and combinations | Custom `/api/v1/lua/synth/step` | SIO input; duration measured in GPU vsyncs |
| Capture application display | `/api/v1/screen/still` | 320 × 240 PNG; excludes native menus and host shader presentation |
| Inspect application RAM | GET `/api/v1/cpu/ram/raw` | Full 2 MiB snapshot; symbols come from the matching `hello.elf` |
| Listening and console timing | Manual / hardware checks | Screenshots and ADSR readbacks do not establish audio quality |

## Run the synth checks

Build the Debug and Release executables using the README, then run from the
repository root:

```sh
python3 scripts/test-web-emulator.py debug
python3 scripts/test-web-emulator.py release --port 8081
```

The runner requires Python's standard library, the existing emulator and BIOS,
and both `hello.exe` and `hello.elf` for the selected configuration. It honors
`PCSX_REDUX` and `PCSX_REDUX_BIOS`; defaults use this project's `.local/` install.
Each launch uses a temporary portable directory with disposable cards. The
runner requires an unused port and terminates its emulator on success or failure.
Interactive emulator settings and cards are not used.

Results go to `build/validation/web-{debug,release}/`: `emulator.log`,
`result.json` with the executable hash and input/frame/state records, and PNG
captures. The runner removes the previous result before starting and replaces
captures with the same names. Use the current exit status and `result.json`
to determine success.

The checks cover:

- Pause/resume and exact completion of bounded pad commands.
- All 64 waveforms selected independently on both operators.
- Prepared bank indices, upper/lower bounds, and L1/R1 steps of ten.
- Triggered program agreement, visible carrier ADSR activity, and envelope completion.
- Highest note, ratio, and depth with the last waveform on both operators.
- PNG format and dimensions, plus invalid-button rejection and scheduler recovery.

`mod-01.png` through `mod-64.png` and `out-01.png` through `out-64.png` capture
each selection. `initial`, `initial-playing`, `maximum-settings`,
`maximum-playing`, and `maximum-stopped` capture startup and envelope behavior.
These images still require visual review for label placement and drawn content.

## Exploratory sessions

Start a separate session from the repository root:

```sh
source scripts/env.sh
mkdir -p build/validation/web-manual
"$PCSX_REDUX" \
  -portable "$PWD/build/validation/web-manual" \
  -no-ui -no-gui-log -stdout -lua_stdout -interpreter \
  -webserver -webserver-port 8080 \
  -bios "$PCSX_REDUX_BIOS" -exe "$PWD/build/debug/hello.exe" \
  -dofile "$PWD/tests/web_control.lua" -run
```

Use a fresh portable directory for a reproducible session. Allow boot to finish
before issuing input; the runner waits for 300 initial vsyncs. Stop the manual
process with Ctrl-C when finished.

From another terminal:

```sh
# Pause and inspect the synth display.
curl --fail -X POST 'http://127.0.0.1:8080/api/v1/execution-flow?function=pause'
curl --fail 'http://127.0.0.1:8080/api/v1/screen/still' -o /tmp/synth.png

# Trigger, then allow neutral frames for release handling and rendering.
curl --fail -X POST \
  'http://127.0.0.1:8080/api/v1/lua/synth/step?buttons=CROSS&hold=4&settle=4'
curl --fail 'http://127.0.0.1:8080/api/v1/lua/synth/status'

# Repeat status until remaining is zero and completed has increased, then capture.
curl --fail 'http://127.0.0.1:8080/api/v1/screen/still' -o /tmp/synth-playing.png

# Release all overrides and pause, including an interrupted command.
curl --fail -X POST 'http://127.0.0.1:8080/api/v1/lua/synth/cancel'
```

`step` accepts `UP`, `DOWN`, `LEFT`, `RIGHT`, `L1`, `R1`, `CROSS`, `CIRCLE`,
`START`, `SELECT`, and comma-separated combinations. Empty `buttons=` advances
neutral frames. `hold` and `settle` are integer vsync counts from 1 through 600.
Commands are asynchronous: submission acknowledges scheduling; `completed`
increases when the command finishes. Wait for completion before submitting
another command. Each command ends with buttons released and emulation paused.
POST `/api/v1/execution-flow?function=resume` resumes continuous execution.

These are video-frame boundaries, not exact instruction or SIO-poll boundaries.
Four-frame taps work below the synth's directional-repeat threshold. Pausing
between steps suits state and display checks; continuous audio needs separate
verification. After `cancel`, resume emulation to let the synth consume release.

## Pinned-version behavior

The project pins PCSX-Redux macOS ARM build 250, commit
`c2e2dec197d3eb8f3db2ee63b8037321dbe1085e`, with bundled OpenBIOS.

- `-webserver -webserver-port PORT` enables the server through CLI.
- `-no-ui` supports both registered Lua handlers and GPU PNG capture.
- POST commands use query parameters. The bridge is loaded at startup and
  remains registered throughout the session; there is no arbitrary-Lua REST endpoint.
- `/api/v1/screen/still` returns the emulated display rather than the desktop.
- GET `/api/v1/cpu/ram/raw` returns the complete emulated RAM snapshot, not a
  requested slice. This runner does not use the RAM-writing POST method.
- The server binds **0.0.0.0** without authentication. A 127.0.0.1 client URL
  does not restrict its listening interface. Use a trusted or network-isolated
  host and stop the server after testing.

Pinned implementation references:
[CLI](https://github.com/grumpycoders/pcsx-redux/blob/c2e2dec197d3eb8f3db2ee63b8037321dbe1085e/src/main/main.cc),
[HTTP dispatch, RAM, binding and PNG](https://github.com/grumpycoders/pcsx-redux/blob/c2e2dec197d3eb8f3db2ee63b8037321dbe1085e/src/core/web-server.cc),
[pad API](https://github.com/grumpycoders/pcsx-redux/blob/c2e2dec197d3eb8f3db2ee63b8037321dbe1085e/src/core/pad.cc).
