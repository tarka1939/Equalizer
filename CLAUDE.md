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

- **The Windows APO likely never applies the configured EQ curve.** In
  `Equalizer/Equalizer.cpp`, `LockForProcess()` and `APOProcess()` each
  declare their own function-local `static DSP::Equalizer10Band s_eq;` —
  these are two *separate* objects that happen to share a name, not a shared
  instance. The one `LockForProcess()` configures with the real band curve
  is never touched again; the one `APOProcess()` actually runs audio through
  is never `Prepare()`'d, so it silently degrades to a passthrough copy. See
  `ARCHITECTURE.md` §7.1 for the full writeup and `Equalizer/tests/test_apo_dsp.cpp`
  / `test_com_exports.cpp` for tests that pin down (not fix) this behavior.
- **`Equalizer::m_gain` defaults to `0.0f`, not the `80%` its own comment
  claims** (`Equalizer.h`: `float m_gain = 0.0f; // 80% volume`). Combined
  with the point above, the shipped APO would currently output silence
  before any explicit gain-setting call exists. See `ARCHITECTURE.md` §7.6.
- **The WASAPI backend is a stub** (`daemon/wasapi_backend.h`) — `Open()`
  just logs and returns `false`; there is no real WASAPI capture/render
  code. Don't assume Windows daemon parity with the Linux/PipeWire path.
  Also: `daemon/CMakeLists.txt`'s `PLATFORM_WINDOWS` branch references a
  `wasapi_backend.cpp` that doesn't exist in the repo — configuring/building
  the daemon on Windows will fail until that source file is added.
- **`daemon/tests/test_wasapi_backend.cpp` builds cross-platform *only
  because* the backend is still a stub** (no real Win32/WASAPI calls in it).
  Once it grows a real implementation, that test target needs to move behind
  `if(PLATFORM_WINDOWS)` in `daemon/CMakeLists.txt` like `eq-daemon` itself —
  it will stop building on Linux/macOS at that point.
- **OverlapAdd engine exists but isn't wired into the live signal path yet**
  (`DSP/OverlapAdd.{h,cpp}`) — it's unit tested in isolation
  (`DSP/tests/test_overlap_add.cpp`) but nothing in `Equalizer/` or `daemon/`
  calls it.
- **This environment (and possibly others used to work on this repo) has no
  Windows SDK, MSVC, or PipeWire dev headers.** The Windows-only test
  projects (`Equalizer/tests/EqualizerRegistryUtilTests.vcxproj`,
  `EqualizerComExportsTests.vcxproj`) and the real `eq-daemon`/PipeWire build
  can't be compiled or run here — only inspected by reading. Don't report
  them as passing/verified without actually building them on a machine that
  has the right toolchain.
