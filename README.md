# System Equalizer

A cross-platform system-level equalizer with three cooperating modules:

| Module | Language | Purpose |
|--------|----------|---------|
| **DSP / daemon** (`daemon/`, `DSP/`) | C++ 17 | Real-time audio processing, IPC server |
| **GUI** (`GUI/`) | C# / Avalonia | 10-band visualiser + settings |
| **CurveGen** (`CurveGen/`) | Python 3.11+ | Room-correction curve generation |

The Windows **APO DLL** (`Equalizer/` → compiled to `Equalizer.dll`) provides the
existing Windows Audio Engine hook. On Linux the daemon uses a **PipeWire filter node**.

> New to this repo? [`ARCHITECTURE.md`](ARCHITECTURE.md) walks through how
> the modules actually fit together (data flow, RT-safety model, IPC
> protocol, known gaps) in more depth than this README.

---

## Build

### C++ daemon + DSP (Linux)

```bash
sudo apt install cmake libpipewire-0.3-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
# Outputs: build/eq-daemon  and  build/WavEqTest
```

### C++ APO DLL (Windows — Visual Studio)

Open `Equalizer.sln` → build **Equalizer** project (Release|x64).

### GUI (cross-platform, requires .NET 8 SDK)

```bash
cd GUI
dotnet run          # development
dotnet publish -c Release   # production
```

### Python CurveGen

```bash
cd CurveGen
pip install -e ".[dev]"
eq-curvegen --help
```

---

## Quick Start (Linux)

```bash
# 1. Start the daemon (will attach to PipeWire)
./build/eq-daemon &

# 2. Generate a room-correction preset from a measurement
eq-curvegen measure --input my_room.wav --output my_preset.json --harman

# 3. Launch the GUI
cd GUI && dotnet run

# 4. In the GUI: "Open Preset" → my_preset.json
#    Or click "Room Correction" to run CurveGen from inside the GUI.
```

## Offline validation via Equalizer APO

The in-repo Windows APO and daemon both have known gaps that make them
unsuitable, today, for validating the curve-generation math against real
playback (see [`ARCHITECTURE.md` §7.1–§7.3](ARCHITECTURE.md#7-known-issues-and-discrepancies)).
As a workaround, `eq-curvegen eqapo` writes the same generated curve as an
[Equalizer APO](https://equalizerapo.com) config file instead of this
project's own JSON preset format:

```bash
eq-curvegen eqapo --input my_room.wav --output my_curve.txt --harman
```

Then install Equalizer APO and either paste `my_curve.txt`'s contents into
its `config.txt`, or reference it with an `Include: <path>` line. See
[`ARCHITECTURE.md` §6](ARCHITECTURE.md#6-curvegen-curvegen) for why this
exists as a separate path rather than an extension of `measure`.

---

## IPC Protocol

The daemon exposes a JSON-line socket at `/tmp/eq-daemon.sock` (Linux) or
`\\.\pipe\eq-daemon` (Windows, future).  See [`shared/ipc_protocol.md`](shared/ipc_protocol.md).

## Preset Format

EQ presets are JSON files conforming to [`shared/preset_schema.json`](shared/preset_schema.json).

---

## Testing

```bash
# C++ (DSP core + daemon protocol/state — builds without PipeWire installed)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure

# Python (CurveGen)
cd CurveGen && pip install -e ".[dev]" && pytest tests/ -v
```

See [`ARCHITECTURE.md`](ARCHITECTURE.md#9-testing-strategy-whats-covered-what-isnt)
for exactly what's covered and what isn't (GUI and the Windows APO/WASAPI paths
currently have no automated tests).

---

## Project Structure

```
Equalizer/
├── CMakeLists.txt          # Root build (DSP lib + daemon + WavEqTest)
├── DSP/                    # Platform-agnostic biquad / 10-band EQ (C++)
│   └── OverlapAdd.{h,cpp}  # FFT block-convolution engine (FIR-filter prep, not yet wired in)
├── daemon/                 # Cross-platform audio daemon (C++)
│   ├── main.cpp
│   ├── eq_state.h          # Lock-free RT↔non-RT state
│   ├── audio_backend.h     # Abstract backend interface
│   ├── ipc_server.{h,cpp}  # Unix socket JSON-line server
│   ├── pipewire_backend.{h,cpp}  # Linux PipeWire filter
│   └── wasapi_backend.h    # Windows WASAPI (stub)
├── GUI/                    # Avalonia C# GUI
│   ├── GUI.csproj
│   ├── Views/
│   │   ├── MainWindow.axaml
│   │   └── EqCurveView.cs  # Custom curve canvas
│   ├── ViewModels/MainViewModel.cs
│   └── Services/IpcClient.cs
├── CurveGen/               # Python acoustic curve generator
│   ├── pyproject.toml
│   └── curvegen/
│       ├── measurement.py    # WAV loading, PSD, smoothing
│       ├── flatten.py        # Inversion + Harman target blend
│       ├── export.py         # JSON preset write/read
│       ├── eqapo_export.py   # Equalizer APO config export (offline validation)
│       └── cli.py            # measure / eqapo / plot / send
├── Equalizer/              # Windows APO DLL (existing)
├── installer/              # Windows INF files
├── shared/
│   ├── preset_schema.json  # JSON Schema for preset files
│   └── ipc_protocol.md     # IPC command reference
└── Tools/WavEqTest.cpp     # Standalone WAV processor
```

---

*Author: Krzysztof Tarka · 2026*
