# CLAUDE.md
This file gives Claude (and other AI coding agents) the context needed to work in this repo. Keep it up to date as the project evolves — treat it like code review feedback the next session shouldn't have to re-learn.

## Project overview

System Equalizer is a cross-platform, system-level audio equalizer with a room-correction module. It has three cooperating pieces plus a legacy Windows hook:

| Module | Path | Language | Purpose |
|---|---|---|---|
| DSP core / daemon | `DSP/`, `daemon/` | C++17 | Real-time audio processing, IPC server |
| GUI | `GUI/` | C# / Avalonia | 10-band visualizer + settings |
| CurveGen | `CurveGen/` | Python 3.11+ | Room-correction curve generation from measurements |
| APO DLL (Windows, legacy) | `Equalizer/` | C++ | Existing Windows Audio Processing Object hook |

On Linux, the daemon runs as a PipeWire filter node. On Windows, WASAPI backend is currently a stub (`daemon/wasapi_backend.h`).

The project goal is creating of audio processing sopftware that would allow for automated room correction curve generation, applying it and refining, in one swift package.
It is targeted for audiophiles and music enjoyers that want to enhance their music listening experience. 

For deeper architectural context (data flow, RT-safety model, IPC protocol, known gaps), see [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Repository map

```
Equalizer/
├── CMakeLists.txt          # Root build: DSP lib + daemon + WavEqTest
├── DSP/                    # Platform-agnostic biquad / 10-band EQ (C++)
│   └── OverlapAdd.{h,cpp}  # FFT block-convolution engine (FIR-filter prep, not yet wired in)
├── daemon/                 # Cross-platform audio daemon (C++)
│   ├── eq_state.h          # Lock-free RT <-> non-RT state
│   ├── audio_backend.h     # Abstract backend interface
│   ├── ipc_server.{h,cpp}  # Unix socket JSON-line server
│   ├── pipewire_backend.{h,cpp}
│   ├── wasapi_backend.h    # Windows WASAPI (stub -- see gotchas below)
│   └── tests/              # eq_state, ipc_server, wasapi_backend (stub) -- all cross-platform
├── GUI/                    # Avalonia C# GUI (MVVM: Views / ViewModels / Services)
├── CurveGen/                # Python acoustic curve generator
│   └── curvegen/
│       ├── measurement.py  # WAV loading, PSD, smoothing
│       ├── flatten.py      # Inversion + Harman target blend
│       ├── export.py       # JSON preset write/read
│       └── cli.py          # measure / plot / send
├── Equalizer/               # Windows APO DLL (legacy)
│   ├── ApoDsp.{h,cpp}      # Per-block gain/EQ/clamp math, extracted from APOProcess for testability (cross-platform)
│   ├── RegistryUtil.{h,cpp} # Registry helpers behind Dll(Un)RegisterServer, parameterized by root HKEY (Windows-only)
│   └── tests/              # test_band_equalizer.cpp / test_apo_dsp.cpp (CMake, cross-platform)
│                            # test_registry_util.cpp / test_com_exports.cpp (Windows-only .vcxproj, in Equalizer.sln)
├── installer/                # Windows INF files
├── shared/
│   ├── preset_schema.json  # JSON Schema for preset files
│   └── ipc_protocol.md     # IPC command reference
└── Tools/WavEqTest.cpp     # Standalone WAV processor
```

## Build & test commands

### C++ daemon + DSP (Linux)
```bash
sudo apt install cmake libpipewire-0.3-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### C++ APO DLL (Windows)
Open `Equalizer.sln` in Visual Studio, build **Equalizer** project (Release|x64).
For the COM/registry unit tests, build and run the **EqualizerRegistryUtilTests**
and **EqualizerComExportsTests** projects in the same solution (Windows-only —
they need real COM/Win32 registry headers, so they can't build via the CMake
path above).

### GUI (.NET 8 SDK required)
```bash
cd GUI
dotnet run                    # dev
dotnet publish -c Release     # production build
```

### CurveGen (Python)
```bash
cd CurveGen
pip install -e ".[dev]"
pytest tests/ -v
eq-curvegen --help
```

## Conventions

- C++: standard is C++17 (per CMakeLists.txt).
- C#: 
- Python:
- Commit messages: [TODO: any format expected, e.g. imperative mood, prefix tags?]
- RT-safety: code in the audio callback path (`DSP/`, `daemon` backends) must not allocate, lock, or block — see `ARCHITECTURE.md` for the RT-safety model before touching this path.

## Git workflow

This repo should be kept clean and current:

- Commit changes regularly with clear, scoped messages — avoid large, unrelated bundles.
- Push working versions (i.e. don't push code that fails the build/test commands above) to `origin/main`. [TODO: confirm branch/PR policy — direct push to main, or feature branches + PR?]
- Before committing, run the relevant build/test command(s) from above for whatever module changed.
- Keep documentation in sync with code changes in the same commit where practical:
  - `README.md` — quick start, build instructions, project structure overview
  - `ARCHITECTURE.md` — data flow, RT-safety model, IPC protocol, known gaps
  - `shared/ipc_protocol.md` — IPC command reference (update when the protocol changes)
  - `CLAUDE.md` (this file) — update when project structure, commands, or conventions change
- `.gitignore` already excludes build artifacts, WAV test fixtures, and IDE files — don't commit generated binaries (`*.exe`, `*.obj`, `*.dll`, `build/`, `bin/`, `obj/`, `x64/`).


## Key docs index

- [`README.md`](README.md) — quick start, build, project structure
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — deep architecture reference
- [`LOCAL_TEST_GUIDE.md`](LOCAL_TEST_GUIDE.md) — Guide to conducting tests
- [`REPORT_APO_INSTALL_ATTEMPTS.md`](REPORT_APO_INSTALL_ATTEMPTS.md) — Description of past problems with installation of APO on Windows machine
- [`shared/ipc_protocol.md`](shared/ipc_protocol.md) — IPC command reference

## What to avoid / gotchas

- **FIXED — the APO's two-`s_eq` bug and zero default gain.** Historically
  `LockForProcess()` and `APOProcess()` each declared their own function-local
  `static DSP::Equalizer10Band s_eq;` — two *separate* objects sharing a name,
  so the configured curve was never the one processing audio — and
  `m_gain` defaulted to `0.0f` behind a `// 80% volume` comment, zeroing every
  sample. The EQ is now a member (`Equalizer::m_eq`) and `m_gain` defaults to
  `1.0f`. Keep it that way: **don't reintroduce a function-local `static` for
  DSP state here** (it is also process-wide shared state across instances —
  latent today, since `u32MaxInstances` is 1, but the wrong shape). Note
  `m_eq` brought a constraint the two separate statics did not have:
  `LockForProcess()`'s `m_eq.Prepare()` reallocates buffers `APOProcess()`
  reads, so **don't add on-the-fly reconfiguration here** without fencing the
  RT callback off — see the comment on the member and `ARCHITECTURE.md` §7.1.
  Pinned by
  `ApoProcess_AppliesTheCurveConfiguredByLockForProcess` in
  `Equalizer/tests/test_com_exports.cpp`, which drives the real
  `LockForProcess()` → `APOProcess()` path and asserts the default curve's
  +3 dB at 62 Hz is audible. `ARCHITECTURE.md` §7.1/§7.6 record the history.
- **The WASAPI backend is a stub** (`daemon/wasapi_backend.h`) — `Open()`
  just logs and returns `false`; there is no real WASAPI capture/render
  code. Don't assume Windows daemon parity with the Linux/PipeWire path.
  The CMake wiring now builds the header-only stub instead of referencing a
  `wasapi_backend.cpp` that never existed. The Windows daemon target does now
  configure, build, link and run (it starts, then exits 1 on
  `"[IPC] Windows named pipe not yet implemented."`) — but note *how*:
  `wasapi_backend.h` defines `CreateAudioBackend()` `inline`, and an inline
  function is only emitted in a translation unit that odr-uses it, so
  `main.cpp` includes that header directly under `#ifdef BACKEND_WASAPI`.
  Listing the `.h` in `DAEMON_SOURCES` does *not* do this — CMake doesn't
  compile headers — and for a while the target configured but failed at link
  with `unresolved external symbol eq::CreateAudioBackend` while this file
  claimed it linked fine. macOS and other platforms skip the `eq-daemon`
  target entirely (no CoreAudio or stub backend source exists), with a
  message rather than a configure-time failure.
- **`daemon/tests/test_wasapi_backend.cpp` builds cross-platform *only
  because* the backend is still a stub** (no real Win32/WASAPI calls in it).
  Once it grows a real implementation, that test target needs to move behind
  `if(PLATFORM_WINDOWS)` in `daemon/CMakeLists.txt` like `eq-daemon` itself —
  it will stop building on Linux/macOS at that point.
- **`daemon/pipewire_backend.cpp` has never been compiled.** No machine used
  on this project has had `libpipewire-0.3-dev`. Reviewing it against the
  actual PipeWire API turned up wrong signatures for both event callbacks and
  a mix-up between the `pw_filter_get_dsp_buffer()` and
  `pw_filter_dequeue_buffer()` APIs — i.e. it could not have built as written.
  Those are corrected *by reading the API*, not by building. Anything
  PipeWire-facing in that file is unproven until someone compiles and runs it
  on Linux. The threading structure around it (RT callback / control thread /
  reconfiguration handshake) is independent of the PipeWire API and is the
  part worth trusting.

- **Nothing that allocates or runs an FFT may be called from an audio
  callback**, and this is easy to get wrong indirectly: `EqPipeline::
  SetBandsPeaking()` and `SetImpulseResponse()` look like simple setters but
  are documented non-RT (transcendental math; a full FFT). `pipewire_backend`
  used to call both straight from `OnProcess()`. They belong on the backend's
  control thread — see `ARCHITECTURE.md` section 4.1.

- **OverlapAdd is wired into the live signal path** via `DSP::EqPipeline`
  (`DSP/EqPipeline.{h,cpp}`), which the PipeWire backend drives. The Windows
  APO (`Equalizer/`) still does not use it — that path is IIR-only.
- **MSVC and the Windows SDK *are* available on this machine** — Visual Studio
  2026 Community (`C:\Program Files\Microsoft Visual Studio\18\Community`),
  MSVC 14.51, Windows Kits 10, plus a CMake and Ninja bundled under
  `Common7\IDE\CommonExtensions\Microsoft\CMake\`. None of them are on `PATH`;
  source `VC\Auxiliary\Build\vcvars64.bat` first and prepend the bundled
  CMake/Ninja directories. This entry previously claimed the opposite, and
  that claim caused real bugs to sit unnoticed — the whole Windows build was
  broken and nobody could see it. Verified working: the full CMake build and
  `ctest` (7 tests), `Equalizer.vcxproj` → `Equalizer.dll`, and both
  Windows-only test projects (`EqualizerRegistryUtilTests` 28 checks,
  `EqualizerComExportsTests` 34 checks, 0 failures). **PipeWire dev headers
  are still absent**, so `pipewire_backend.cpp` remains uncompiled — see the
  entry above.
- **Both Windows `.vcxproj` files need `<LanguageStandard>stdcpp17</LanguageStandard>`
  and `runtimeobject.lib`.** MSVC defaults to C++14, so without the first the
  build dies on `Diagnostics.h`'s `inline` variables (`error C7525`); without
  the second it dies at link on `RoOriginateError`, which WRL's
  `<wrl/module.h>` (used by `DllCanUnloadNow`) references. Both are set now in
  all four configurations of each project — don't drop them, and add them to
  any new `.vcxproj`.
- **Headers go in `<ClInclude>`, never `<ClCompile>`, in a `.vcxproj`.**
  `Equalizer.h` sat in `<ClCompile>` for a long time, so MSBuild handed it to
  `cl.exe` as a `/TP` translation unit whose object (`Equalizer.obj`) landed
  on the *same* `/Fo` path as the real one from `Equalizer.cpp`. Whichever was
  written last won, which made the DLL link fail **intermittently** with
  `unresolved external symbol CLSID_Equalizer` (plus the constructor and
  `GetRegistrationProperties`). A full `/t:Rebuild` could succeed while the
  next incremental build failed — easy to misdiagnose as a stale-object fluke.
- **`daemon/tests/test_ipc_server.cpp` is POSIX-only** — it drives a real Unix
  domain socket, matching `ipc_server.cpp`'s own `#ifndef _WIN32` body. Its
  target is guarded by `if(NOT PLATFORM_WINDOWS)` in `daemon/CMakeLists.txt`;
  it was previously unguarded, which broke the entire Windows CMake build on
  `'sys/socket.h': No such file or directory`. Unguard it when the Windows
  named-pipe implementation lands (`ARCHITECTURE.md` §7.2).
