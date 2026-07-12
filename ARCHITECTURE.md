# Architecture

This document describes how System Equalizer actually works internally: the
data flow through each module, the concurrency/RT-safety model, the IPC
protocol, and the build/test setup. It's aimed at someone who has never seen
the codebase before. For build commands and a directory listing, see
[README.md](README.md). For the academic writeup and design rationale, see
the project report.

---

## 1. System map

The project is really **two separate systems that share a DSP core**, plus a
Python tool that feeds both of them presets:

```
                    ┌─────────────────────────┐
                    │   DSP/  (C++ header lib)  │
                    │   Biquad + Equalizer10Band │
                    └───────────┬────────────┬──┘
                                │            │
              ┌─────────────────┘            └─────────────────┐
              ▼                                                 ▼
  ┌───────────────────────────┐                 ┌───────────────────────────┐
  │  Equalizer/  (Windows)     │                 │  daemon/  (cross-platform) │
  │  Windows Audio Processing  │                 │  Unix socket / named pipe  │
  │  Object (APO), COM, WASAPI │                 │  IPC server + PipeWire     │
  │  → Equalizer.dll           │                 │  (Linux) backend           │
  └───────────────────────────┘                 └──────────────┬────────────┘
                                                                  │ JSON-line IPC
                                                                  ▼
                                                    ┌───────────────────────────┐
                                                    │  GUI/  (Avalonia / C#)     │
                                                    │  10-band sliders, presets  │
                                                    └───────────────────────────┘

              ┌───────────────────────────┐
              │  CurveGen/  (Python)       │   measurement → smoothing →
              │  mic recording → preset    │   inversion → JSON preset,
              │  JSON, independent of      │   optionally pushed to the
              │  which host runs the DSP   │   daemon over the same IPC
              └───────────────────────────┘   protocol (`eq-curvegen send`)
```

**Important, and easy to miss:** the Windows APO path and the daemon path are
two independent implementations of "host the DSP core in real time," not one
system running on two platforms. They don't share process, state, or IPC —
each links `DSP/` directly and does its own thing. The GUI and CurveGen's
`send` command only talk to the **daemon**, over the JSON-line socket. On
Windows, there is currently no bridge between the GUI/CurveGen and the APO
DLL — see [§7 Known issues](#7-known-issues-and-discrepancies).

---

## 2. DSP core (`DSP/`)

This is the only part of the codebase both hosts actually share. It has no
knowledge of COM, WASAPI, PipeWire, or sockets — just float buffers in, float
buffers out.

### 2.1 `Biquad` — one RBJ peaking filter

A biquad is a 2nd-order IIR filter:

```
y[n] = b0·x[n] + b1·x[n-1] + b2·x[n-2] − a1·y[n-1] − a2·y[n-2]
```

`SetPeaking(centerHz, Q, gainDb)` derives `{b0,b1,b2,a1,a2}` from the standard
RBJ ("Audio EQ Cookbook") peaking-EQ formula:

```
omega = 2π·centerHz / sampleRate
alpha = sin(omega) / (2·Q)
A     = 10^(gainDb / 40)          # sqrt of the linear power gain

b0 = 1 + alpha·A       a0 = 1 + alpha/A
b1 = −2·cos(omega)     a1 = −2·cos(omega)
b2 = 1 − alpha·A       a2 = 1 − alpha/A

# stored coefficients are normalized so a0 = 1:
c.b0 = b0/a0,  c.b1 = b1/a0,  c.b2 = b2/a0,  c.a1 = a1/a0,  c.a2 = a2/a0
```

A useful, non-obvious property that the test suite (`DSP/tests/test_biquad.cpp`)
relies on: at **0 dB gain**, `A = 1`, which makes `b1 == a1` and `b2 == a2`
exactly (both are `-2cos(omega)/a0` and `(1∓alpha)/a0` respectively) — so the
transfer function's numerator and denominator are identical and `H(z) = 1`
exactly. A 0 dB peaking band is provably a perfect pass-through, not just
"close to unity." This is the basis for `Biquad_ZeroDbPeakingIsPassthrough`
and `Equalizer10Band_AllZeroGainsIsPassthrough`.

**RT-safety.** `Biquad` is designed to be reconfigured from a non-real-time
thread while a real-time audio callback is concurrently calling `Process()`:

- Coefficients live in a 2-element array (`m_coeffs[2]`), double-buffered.
- `SetCoefficients()` (non-RT) writes into the *inactive* slot, then atomically
  flips `m_activeIndex` (`memory_order_release`).
- `Process()` (RT) reads `m_activeIndex` once per call (`memory_order_acquire`)
  and uses that snapshot for the whole buffer — no locks, no allocation, no
  torn reads of a coefficient set.
- Per-channel filter history (`x1,x2,y1,y2`) lives in `m_states`, sized once in
  `Prepare()`. `Process()` never resizes or allocates.

This is the same double-buffer-plus-atomic-index pattern used one level up in
`eq::EqState` for gains (§4.1) — the whole codebase leans on this one
technique for RT/non-RT handoff rather than locks.

### 2.2 `Equalizer10Band` — 10 cascaded `Biquad`s

Fixed at 10 bands (`BandCount = 10`), standard ISO-ish centres
`{31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000}` Hz, one shared `Q`
across all bands. `Process()` runs the bands back-to-back, each one's output
feeding the next (`m_bands[0].Process(input, output, …)` then
`m_bands[i].Process(output, output, …)` for `i = 1..9`), so it works correctly
whether `input == output` (in-place) or not — verified by
`Equalizer10Band_InPlaceMatchesOutOfPlace`.

**Behavior when never `Prepare()`'d** (`m_channels == 0`): `Process()`
degrades to a straight `memcpy`-equivalent copy from input to output, doing
*no* filtering at all, rather than crashing or silently doing nothing. This
is intentional-looking defensive code — but see §7.1, because this exact
"unprepared instance quietly no-ops" behavior is what makes a real bug in
the Windows APO invisible.

---

## 3. Windows APO (`Equalizer/`)

A Windows Audio Processing Object: a COM component (`CLSID_Equalizer`) that
`audiodg.exe` loads directly into the system audio engine after it's
registered (`regsvr32`) and wired to a render endpoint's `FxProperties`
(see `LOCAL_TEST_GUIDE.md` for the full registry dance — this is the part of
the project that consumed the most debugging effort; see
`REPORT_APO_INSTALL_ATTEMPTS.md`).

Two COM entry points matter:

- **`LockForProcess()`** — called once when the audio engine locks the format.
  Reads the negotiated sample rate/channel count off `WAVEFORMATEX`, then
  builds a `DSP::Equalizer10Band`, calls `Prepare()` and
  `SetBandsPeaking()` with `BandEqualizer`'s default V-shaped curve, and
  `Reset()`s it.
- **`APOProcess()`** — called on the real-time thread for every audio buffer.
  Applies a preamp gain, runs the signal through an `Equalizer10Band`, then
  hard-clamps to `[-1, 1]`.

### 7.1 goes here — see below; this is flagged, not glossed over, because it
directly affects whether the "APO applies the configured curve" claim in the
thesis report is actually true of the shipped code. Read §7.1.

---

## 4. Daemon (`daemon/`)

The cross-platform alternative to the APO: a standalone process
(`eq-daemon`) that opens its own audio backend and exposes a small JSON-line
control protocol.

### 4.1 `EqState` — the RT/non-RT handoff

```cpp
struct EqState {
    std::array<float, 10> pending_gains;   // written by IPC thread
    std::atomic<bool>     pending_dirty;
    std::atomic<float>    preamp_db;
    std::atomic<bool>     enabled;

    std::array<float, 10> current_gains;   // for get_state reads only
    float sample_rate; uint32_t channels;

    void SetGains(gains)                 { pending_gains = gains; pending_dirty.store(true, release); }
    bool ConsumePending(std::array&out)  { if (!dirty.load(acquire)) return false;
                                            dirty.store(false, release); out = pending_gains; return true; }
};
```

Same double-buffer-and-flag pattern as `Biquad`'s coefficient swap, one level
up: the IPC thread calls `SetGains()`, the audio callback polls
`ConsumePending()` once per buffer and only re-configures the `Equalizer10Band`
when something actually changed.

**A real seam worth knowing about** (covered explicitly by
`EqState_SetGainsMarksDirtyAndStoresPending` in
`daemon/tests/test_eq_state.cpp`): `SetGains()` only ever touches
`pending_gains`. `current_gains` — the array `get_state` reports back to
clients — is updated separately, by `IpcServer::ProcessCommand`, *after* a
successful `set_bands` (`m_state->current_gains = gains;` in
`ipc_server.cpp`). `EqState` itself does not keep these two arrays in sync;
if you ever call `SetGains()` from anywhere other than the `set_bands` IPC
handler, `get_state` will silently report stale values.

### 4.2 `IpcServer` — the JSON-line protocol

Full protocol reference: [`shared/ipc_protocol.md`](shared/ipc_protocol.md).
Implementation notes not obvious from the protocol doc:

- **Transport**: Unix domain socket at `/tmp/eq-daemon.sock` on Linux/macOS.
  The Windows named-pipe path is a declared constant
  (`kPipeName = \\.\pipe\eq-daemon`) but `Start()` just logs "not yet
  implemented" and returns `false` on `_WIN32` — there is no Windows
  implementation of the server side of this protocol yet.
- **JSON parsing is hand-rolled**, not a library: `ipc_server.cpp` has its own
  `JsonGetString`/`JsonGetFloat`/`JsonGetBool`/`JsonGetGains` that do
  substring search rather than real parsing. This is fine for the fixed,
  small command set it currently handles, but it is not a general JSON
  parser — malformed-but-plausible-looking input can confuse it in ways a
  real parser wouldn't (e.g. it has no concept of nested objects or string
  escaping).
- **Threading**: `AcceptLoop()` runs on its own thread, polling the listening
  socket every 200ms (so `Stop()` can unblock it); each accepted client is
  handled on its own **detached** thread (`HandleClient`), which loops
  reading newline-delimited commands until the client disconnects.
- **`load_preset` is a stub.** It validates that a `path` was given and then
  unconditionally returns `{"ok":false,"error":"load_preset not yet
  implemented"}` — reading the file and applying it is a TODO in the source.
  Covered explicitly by `IpcServer_LoadPresetIsNotYetImplemented` so this
  regresses loudly (as a test failure, which is good news) if someone
  implements it without updating the test.

Verified end-to-end in `daemon/tests/test_ipc_server.cpp` by actually
connecting a client over the real Unix socket (not by calling
`ProcessCommand` directly, even though that would be simpler) — this
exercises the real accept/thread/socket code path, not just the JSON logic.

### 4.3 Audio backends (`audio_backend.h` + platform `.cpp`)

`AudioBackend` is a tiny abstract interface (`Open()`, `Close()`, `Name()`);
`CreateAudioBackend()` is a factory implemented once per platform, chosen at
compile time via `BACKEND_PIPEWIRE` / `BACKEND_WASAPI` / `BACKEND_COREAUDIO`
(set by `daemon/CMakeLists.txt` based on `PLATFORM_LINUX` /
`PLATFORM_WINDOWS` / `PLATFORM_MAC`).

- **PipeWire (Linux) — implemented.** `pipewire_backend.cpp` registers a
  PipeWire *filter* node with 2 input + 2 output ports, interleaves the
  planar buffers PipeWire hands it, runs them through an
  `Equalizer10Band`, applies preamp + `[-1,1]` clamp, and de-interleaves back
  out. Gains are pulled from `EqState::ConsumePending()` once per RT
  callback (`OnProcess`).
- **WASAPI (Windows) — declared but not implemented.** `wasapi_backend.h`
  exists as a header; there is no corresponding `.cpp`, and
  `daemon/CMakeLists.txt`'s Windows branch references
  `wasapi_backend.cpp` as a source file that does not exist in the repo.
  **The daemon does not currently build on Windows.**
- **CoreAudio (macOS) — same situation**: header/CMake wiring exists,
  implementation doesn't.

So today, `eq-daemon` is a Linux-only, PipeWire-only binary in practice,
despite the CMake scaffolding for three platforms.

---

## 5. GUI (`GUI/`)

Avalonia 11 / .NET 8, MVVM via ReactiveUI. Talks to the daemon over the same
IPC protocol as CurveGen's `send` command — it is a thin client, it does not
link `DSP/` itself.

- **`IpcClient`** (`Services/IpcClient.cs`): connects a `UnixDomainSocketEndPoint`
  to `/tmp/eq-daemon.sock`. On Windows, `ConnectAsync()` immediately returns
  `false` (`// TODO: implement Named Pipe client`) — matching the daemon's
  own unimplemented Windows IPC side from §4.2. Both halves of the
  Windows IPC path are stubs right now.
- **`MainViewModel`**: holds 10 `BandViewModel`s (each clamped to ±12 dB) plus
  preamp (±20 dB) and an enabled toggle. Band-gain changes are merged,
  **throttled 120ms**, and pushed as a single `set_bands` call — so dragging
  a slider doesn't flood the daemon with a command per pixel of drag.
  Preamp and enabled changes are throttled/pushed independently.
- **Preset I/O** is direct JSON file read/write via `System.Text.Json`
  against `GUI/Models/EqPreset.cs` — it does not go through
  `CurveGen/curvegen/export.py`'s Python schema logic, but both are meant to
  conform to the same [`shared/preset_schema.json`](shared/preset_schema.json).
  Nothing enforces that the two independent (de)serializers stay compatible
  beyond manual convention.
- **"Room Correction" button** (`RunCurveGenAsync`) shells out to the
  `eq-curvegen` CLI as a subprocess (`measure --input <wav> --output <tmp>
  --harman`), then reads the resulting preset JSON back in. This is the only
  connection between the GUI and CurveGen — there's no in-process Python
  interop, just a CLI call and a shared JSON file.

---

## 6. CurveGen (`CurveGen/`)

Python 3, the room-correction measurement/curve-generation pipeline. Four
modules, each independently testable and now independently tested:

```
measurement.py          flatten.py              export.py           cli.py
──────────────          ──────────              ─────────           ──────
load_wav /               compute_correction:      write_preset /      measure / plot / send
load_impulse_response    invert + reference to    read_preset /
  → (freqs, mag_db, sr)    1kHz + optional Harman  preset_to_gains
smooth_octave              blend + clip + preamp
```

- **`measurement.py`**: `load_wav` uses Welch's method (`scipy.signal.welch`,
  `nperseg=4096`) for a PSD estimate off a raw recording; `load_impulse_response`
  instead does a windowed FFT, for use with a pre-computed impulse response
  (e.g. from REW). Both normalize integer PCM to float32 first
  (`_norm_audio`: int16/32 divide by full-scale, uint8 is offset-and-scaled
  since WAV's 8-bit PCM is unsigned). `smooth_octave` applies fractional-octave
  averaging in log-frequency space, band-by-band, with a hard skip for `f ≤ 0`
  (the DC bin) to avoid `log10(0)`.

- **`flatten.py`**: `compute_correction` inverts the measured response
  (`-meas_at_bands`) and then **references everything to 0 dB at 1 kHz**:

  ```python
  ref_1khz_db = np.interp(np.log10(1000.0), log_freqs, magnitude_db)
  gains_db += ref_1khz_db     # gain(f) = level(1kHz) - level(f)
  ```

  This reference step exists because `measurement.py`'s output is on an
  *arbitrary, uncalibrated* scale (Welch PSD units, or raw FFT magnitude —
  not dBFS or SPL), so there's no meaningful absolute "0 dB" without picking
  a convention. 1 kHz is the standard audio-engineering reference point.

  **This means a real defect located exactly at 1 kHz is invisible to this
  algorithm by construction** — the band at the reference frequency always
  corrects to ~0, regardless of what's actually happening there, because
  everything is measured *relative to* that band. This isn't a bug so much
  as an inherent limitation of anchoring to a single reference frequency
  without independent calibration; it's covered explicitly by
  `test_boost_at_reference_frequency_is_self_cancelling` in
  `tests/test_flatten.py`, specifically so the behavior stays visible and
  intentional rather than being rediscovered as a surprise later.

  *(Historical note: this reference step had a sign bug — `-=` instead of
  `+=` — that made every band's correction saturate to the clip limit on
  real measurements. Fixed; see git history and the "Testing" section
  below for how it was caught.)*

- **`export.py`**: reads/writes the JSON preset format defined by
  [`shared/preset_schema.json`](shared/preset_schema.json) — `write_preset`
  doesn't itself validate against the JSON Schema (no `jsonschema.validate`
  call despite the dependency being present in `pyproject.toml`), it just
  hand-checks that `len(gains_db) == len(band_hz)`.

- **`cli.py`**: `measure` runs the full pipeline and writes a preset;
  `plot` does the same plus a matplotlib chart (raw vs. smoothed response,
  bar chart of band gains); `send` reads a preset back and pushes it to a
  running daemon over the same Unix socket / JSON-line protocol as the GUI
  (`set_preamp` then `set_bands`), independent of the GUI entirely.

---

## 7. Known issues and discrepancies

Things worth knowing before trusting a claim about what this system does,
found while writing tests and this document rather than assumed from the
report.

### 7.1 The Windows APO likely never applies the configured EQ curve (HIGH SEVERITY, unconfirmed on real hardware)

In `Equalizer/Equalizer.cpp`:

```cpp
HRESULT Equalizer::LockForProcess(...) {
    ...
    static DSP::Equalizer10Band s_eq;      // function-local static #1
    s_eq.Prepare(sampleRate, channels);
    s_eq.SetBandsPeaking(centers, gains, 1.0f);
    s_eq.Reset();
    return S_OK;
}

void Equalizer::APOProcess(...) {
    ...
    static DSP::Equalizer10Band s_eq;      // function-local static #2 — DIFFERENT OBJECT
    s_eq.Process(out, out, frameCount, channels);
    ...
}
```

`static` inside a function scopes the variable's *lifetime*, not its
*identity across functions* — `LockForProcess`'s `s_eq` and `APOProcess`'s
`s_eq` are two entirely separate `Equalizer10Band` instances that happen to
share a name. The one that gets configured with the actual band curve
(`LockForProcess`) is never touched again. The one that actually processes
audio (`APOProcess`) is never `Prepare()`'d, so per §2.2's documented
behavior for an unprepared instance (`m_channels == 0`), its `Process()`
call is a silent pass-through copy — no filtering happens at all. The only
audible effect `APOProcess` has is the preamp multiply and the `[-1,1]`
safety clamp.

This was found by reading the code closely while writing
`Equalizer10Band_UnpreparedActsAsPassthrough` (in
`DSP/tests/test_biquad.cpp`) — that test exists specifically to pin down
*why* this pass-through-on-unprepared behavior matters, not just that it's
mildly convenient default behavior. It could not be verified against real
Windows audio hardware in this environment (no Windows/COM/WASAPI available
here), so treat this as a strong, specific, textually-verifiable finding
rather than a confirmed field observation — but the code reads
unambiguously. If real listening tests ever suggested "the APO's EQ curve
doesn't seem to do anything," this is almost certainly why.

### 7.2 Windows IPC is a stub on both ends

`daemon/ipc_server.cpp::Start()` refuses to start on `_WIN32`
(`"[IPC] Windows named pipe not yet implemented."`), and
`GUI/Services/IpcClient.cs::ConnectAsync()` immediately returns `false` on
Windows (`// TODO: implement Named Pipe client`). Neither side of the
protocol exists for Windows yet, so the GUI cannot currently control
anything on Windows — not the daemon (which also doesn't build there; see
§7.3) and not the APO DLL (which has no IPC of any kind, see §3).

### 7.3 `eq-daemon` does not build on Windows or macOS

`daemon/CMakeLists.txt`'s `PLATFORM_WINDOWS` and `PLATFORM_MAC` branches
reference `wasapi_backend.cpp` / `coreaudio_backend.cpp` as build sources;
only the `.h` headers exist in the repo. Configuring CMake on those
platforms would fail at the generation step once it tries to add those
missing source files (or fail at compile/link time, depending on CMake
version behavior). The daemon is Linux/PipeWire-only in its current state.

### 7.4 `flatten.py`'s reference-frequency limitation

Covered above in §6 — not re-stating it here, just cross-referencing so
anyone skimming "Known issues" for a list doesn't miss it.

### 7.5 Two independent preset (de)serializers

`GUI/Models/EqPreset.cs` (`System.Text.Json`) and
`CurveGen/curvegen/export.py` (Python `json`) both read/write files meant to
conform to `shared/preset_schema.json`, but neither actually validates
against that schema, and there is no shared test asserting the two
serializers stay compatible. A schema change in one place is not guaranteed
to be caught by the other.

---

## 8. Build system

| Component | Tool | Entry point |
|---|---|---|
| DSP lib + `eq-daemon` + `WavEqTest` + all new test executables | CMake ≥ 3.20 | root `CMakeLists.txt` → `daemon/CMakeLists.txt` |
| Windows APO DLL | MSBuild / Visual Studio | `Equalizer.sln` |
| GUI | .NET 8 SDK | `GUI/GUI.csproj` |
| CurveGen | pip / setuptools | `CurveGen/pyproject.toml` |

The CMake side now has an `EQUALIZER_BUILD_TESTS` option (default `ON`) that
adds three test executables alongside the existing ones:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

`dsp_tests` (DSP core) and `eq_state_tests` / `ipc_server_tests` (daemon
protocol/state) build independently of whether `libpipewire-0.3-dev` is
installed — they were deliberately kept free of the PipeWire dependency so
they still build and run in minimal environments (including the one this
document was written in, which had no `cmake` binary at all and no
PipeWire dev package; every test in this document was verified with direct
`g++ -std=c++17 -Wall -Wextra` invocations mirroring exactly what the CMake
targets above declare, not assumed from reading the CMake files).

---

## 9. Testing strategy: what's covered, what isn't

### Covered

| Area | File(s) | What it checks |
|---|---|---|
| Biquad math | `DSP/tests/test_biquad.cpp` | Unity/0dB-passthrough identity, RBJ gain accuracy at center frequency (±0.1–0.15 linear tolerance), multi-channel state isolation, `Reset()` clearing feedback state |
| 10-band cascade | `DSP/tests/test_biquad.cpp` | Unprepared-instance passthrough (§7.1's root cause), all-zero-gain passthrough, single-band boost, in-place vs. out-of-place equivalence |
| RT/non-RT gain handoff | `daemon/tests/test_eq_state.cpp` | Default state, dirty-flag semantics, single-consume behavior, `pending_gains`/`current_gains` non-synchronization (§4.1) |
| IPC protocol | `daemon/tests/test_ipc_server.cpp` | `set_bands` (valid + wrong-length), `set_preamp`, `set_enabled`, `get_state`, `load_preset` (documents stub), unknown command, empty line — all over a real Unix socket |
| WAV/PSD/FFT measurement | `CurveGen/tests/test_measurement.py` | PCM normalization (int16/int32/uint8/float32/float64), peak-frequency accuracy for both Welch-PSD and FFT-IR paths, mono/stereo channel selection incl. modulo-wraparound, fractional-octave smoothing (DC-bin safety, spike smoothing, fraction sensitivity) |
| Correction-curve math | `CurveGen/tests/test_flatten.py` | Flat-input/zero-correction, boost inversion, 1kHz self-cancellation (§6), clipping, auto-preamp sign, Harman blending, arbitrary band counts |
| Preset serialization | `CurveGen/tests/test_export.py` | Round-trip, schema-mismatch errors, directory creation |

Run everything:

```bash
# C++ (from a configured CMake build directory)
ctest --output-on-failure
# — or, without CMake, compile+link each test .cpp directly against the
#   sources it needs (see the header comment atop each test file for the
#   exact g++ invocation used to verify it in this environment)

# Python
cd CurveGen && pip install -e ".[dev]" && pytest tests/ -v
```

### Not covered (and why)

- **The Windows APO DLL itself** (`Equalizer/`) — requires COM/WASAPI headers
  and a Windows toolchain; nothing in this repo exercises `APOProcess()` or
  `LockForProcess()` directly. §7.1 was found by code reading, not by a test
  that runs the APO.
- **`pipewire_backend.cpp`** — requires a running PipeWire graph (or at least
  `libpipewire-0.3-dev` to compile); untested here for the same reason the
  daemon couldn't be built end-to-end in this environment.
- **`wasapi_backend.h` / `coreaudio_backend.h`** — no corresponding `.cpp`
  exists to test (§7.3).
- **The GUI** (`GUI/`) — no .NET SDK was available in the environment this
  was written in, and there is no existing GUI test project to extend. The
  ViewModel logic (throttling, clamping, preset round-trip) is a reasonable
  next target if a test project is added.
- **`Tools/WavEqTest.cpp`** — this is a manual inspection CLI, not something
  with pass/fail assertions; it's still useful as a quick end-to-end sanity
  check (`WavEqTest in.wav out.wav`) and is exercised that way in
  `LOCAL_TEST_GUIDE.md`-style manual testing, but it isn't part of the
  automated suite.

---

*This document describes the codebase as of the commit it was added in. If
you fix §7.1–§7.3, please update this file in the same change — that's the
whole point of keeping it next to the code instead of in the thesis PDF.*
