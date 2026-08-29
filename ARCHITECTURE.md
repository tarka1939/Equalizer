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
DLL — see [§7 Known issues](#7-known-issues-and-discrepancies). CurveGen also
has a third, *offline* output path (`eq-curvegen eqapo`) that bypasses both
the APO and the daemon entirely — see §6.

For a step-by-step view of what actually executes, in order, see the two
diagrams in [`docs/diagrams/`](docs/diagrams/):
[`dsp_execution_pipeline.svg`](docs/diagrams/dsp_execution_pipeline.svg)
(the RT audio path plus the Windows APO/WavEqTest/OverlapAdd side paths, §2
and §7.1) and
[`curvegen_data_flow_pipeline.svg`](docs/diagrams/curvegen_data_flow_pipeline.svg)
(measurement → correction → preset, §6, including the two feature-branch
extensions that aren't merged into `main` yet).

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

### 2.3 `OverlapAdd` — FFT block convolution (FIR engine)

**Status: wired in, on the Linux daemon and `WavEqTest` only.** As of
`DSP/EqPipeline.{h,cpp}` (§2.4), `OverlapAdd` is a real stage in the
execution pipeline for those two hosts — see §2.4 for the FIR/IIR
ordering rules. It is still **not** wired into the Windows APO
(`Equalizer/`, §3) or the WASAPI/CoreAudio backends, which continue to
use `Equalizer10Band` alone, unchanged. It exists (and was originally
written) so a FIR-filter feature — measured room-correction impulse
responses, linear-phase crossovers, etc. — had somewhere to plug in
without redesigning the convolution machinery from scratch; the Windows
APO project already hinted at this (`Equalizer.h` has a long-standing
commented-out `#include "kiss_fftr.h"  // if using real FFT`, and
`kiss_fft.c`/`kiss_fftr.c` were already compiled into `Equalizer.vcxproj`
but not into this cross-platform CMake build until it was first added).

`OverlapAdd` implements the classic block-based FFT convolution algorithm
("Overlap-Add"), built on the vendored **KissFFT** (`Equalizer/kissfft-131.2.0/`,
BSD-3-Clause; only `kiss_fft.c`/`kiss_fftr.c`, the real-FFT subset, are
compiled — not `kiss_fftnd`/`kfc`, which nothing here needs):

- Input arrives as a continuous stream and is grouped internally into fixed
  analysis blocks of `blockSize` (B) frames.
- Each block is zero-padded to `fftSize` (N), a size with only `{2,3,5}` as
  prime factors and `N ≥ B + M − 1` (M = the largest impulse response
  `SetImpulseResponse()` will ever be given), then transformed with
  `kiss_fftr`.
- The transformed block is multiplied bin-by-bin against the filter's
  precomputed spectrum and inverse-transformed (`kiss_fftri`). Because
  `N ≥ B + M − 1`, this per-block *circular* convolution equals the true
  *linear* convolution of that block against the filter — no wraparound.
- Consecutive blocks' "tails" (the `N − B` samples of each inverse-FFT result
  that extend past the current block) are carried forward and added into the
  next block's output — the "Add" in Overlap-Add — which is what reassembles
  a continuous linear convolution out of independent per-block circular
  convolutions.

**Normalization.** KissFFT's `kiss_fftr`/`kiss_fftri` pair is *not*
individually normalized — a forward+inverse round trip scales every sample
by exactly N (confirmed empirically by compiling the vendored sources
against a standalone round-trip check, not assumed from the docs). Rather
than pay a per-sample division in the RT path, `SetImpulseResponse()`
(non-RT) pre-scales the filter's spectrum by `1/N` once: if `X = kiss_fftr(block)`
and `H = kiss_fftr(padded taps)`, using `H' = H/N` gives
`kiss_fftri(X · H') == true linear-convolution output` directly, with zero
extra arithmetic in `Process()`.

**RT-safety** follows the same pattern as `Biquad` (§2.1): the filter's
frequency-domain representation is double-buffered
(`m_filterSpectrum[2]`) behind an `std::atomic<uint32_t>` index, written
with `memory_order_release` by `SetImpulseResponse()` (non-RT) and read with
`memory_order_acquire` once per completed block by the RT path. `Process()`
itself never allocates — all buffers (per-channel block/spectrum/tail/ring
storage) are sized once in `Prepare()`. KissFFT's own transform calls are
allocation-free too for every realistic `fftSize` this class produces:
tracing `kf_factor()`/`kf_work()` in `kiss_fft.c` shows that for any
`{2,3,5}`-smooth size ≥ 2, every FFT stage dispatches on radix 2, 3, 4, or 5
— never KissFFT's one internal path that can call `alloca()`/`malloc()` per
transform (`kf_bfly_generic()`, used only for other radices). The sole
exception is the degenerate `fftSize == 2` case (`blockSize == maxImpulseLength == 1`),
which is irrelevant for any real audio block size. This is verified line by
line against `kiss_fft.c`, and cross-checked by
`OverlapAdd_FftSizeIsAlwaysTwoThreeFiveSmooth` in the test suite below —
not just asserted in a comment.

**Latency.** `GetLatencySamples()` returns exactly `blockSize − 1`: a block's
convolution can't be computed until all `blockSize` of its input samples
have arrived, so the first `blockSize − 1` output samples of the stream are
silence. `Process(frames, ...)` accepts an arbitrary, non-block-aligned, and
call-to-call-varying `frames` count — internal staging and a small output
ring buffer (`4× blockSize` capacity; occupancy is bounded well below that,
see the comment in `OverlapAdd::Process()`) absorb the difference between
however the caller chops audio and the fixed analysis block size.

`SetImpulseResponse()` applies the same FIR to every channel (no
per-channel filter support yet — not needed until something actually wires
this engine into a signal chain).

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

The gain/EQ/clamp math inside `APOProcess()` now lives in
`ApoDsp::ProcessBlock()` (`Equalizer/ApoDsp.{h,cpp}`) — pulled out specifically
so it has no dependency on `APO_CONNECTION_PROPERTY`/COM and can be unit
tested cross-platform (`Equalizer/tests/test_apo_dsp.cpp`, part of the root
CMake build). `APOProcess()` itself is now a thin wrapper that unpacks the
connection buffers and calls it — no behavior change. Similarly, the
registry-writing helpers behind `DllRegisterServer`/`DllUnregisterServer`
(`ComExports.cpp`) now live in `Equalizer/RegistryUtil.{h,cpp}`, parameterized
by root `HKEY` and key path so they can be tested against a
`HKEY_CURRENT_USER` scratch key instead of the real `HKEY_LOCAL_MACHINE`
registration. See §9 for what's actually exercised where.

See §7.1 for a specific, high-severity finding about whether the configured
EQ curve is actually applied by the shipped code.

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

**Who consumes this, and why it is not lock-free any more.** It used to be
described as the same double-buffer-and-flag pattern as `Biquad`'s coefficient
swap, with the *audio callback* polling `ConsumePending()` once per buffer.
Two things were wrong with that:

1. It was not a double buffer. There is one `pending_gains` array (and one
   `pending_ir`), so a second `SetGains()` could overwrite it while the reader
   was mid-copy -- a data race, not merely a stale read.
2. Consuming on the RT thread meant the RT thread also had to *apply* the
   result, and applying means `SetBandsPeaking()` (10x `sin`/`cos`/`pow`) or
   `SetImpulseResponse()` (a heap allocation and a full FFT). Both are
   documented non-RT. Every `set_fir` command allocated inside the audio
   callback.

So the consumer is now a dedicated **control thread** owned by the audio
backend (`PipeWireBackend::ControlLoop`, section 5.1). The RT callback reads
only the `enabled` and `preamp_db` atomics. With both ends of the handoff off
the RT path, the pending arrays are guarded by an ordinary mutex, and the
control thread blocks on `EqState::WaitForUpdate()` rather than polling.
`ConsumePending*()`'s once-then-false semantics are unchanged.

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

- **PipeWire (Linux) — written, never compiled.** `pipewire_backend.cpp`
  registers a PipeWire *filter* node with 2 input + 2 output ports,
  interleaves the planar buffers PipeWire hands it, runs them through the
  `EqPipeline` (FIR then IIR), applies preamp + a NaN-folding `[-1,1]` clamp,
  and de-interleaves back out.

  Threading is a three-way split: the RT callback (`OnProcess`) does audio and
  nothing else; a **control thread** (`ControlLoop`) owns every non-RT DSP
  call — `Prepare`, `SetBandsPeaking`, `SetImpulseResponse` — and is what
  consumes `EqState`'s pending gains/IR; and `OnParamChanged` only records the
  new sample rate, handing the re-`Prepare()` to the control thread so all
  reconfiguration happens on one thread. Because `Prepare()` reallocates the
  buffers `Process()` reads, it runs behind a `BeginReconfigure()` /
  `EndReconfigure()` handshake (a Dekker pair of seq_cst flags) during which
  the RT thread falls back to passthrough. Nothing else needs the handshake:
  the other setters publish through the existing atomic slot swaps.

  **Caveat — this file has never been built.** No development machine used on
  this project has had `libpipewire-0.3-dev`. Reviewing it against the
  PipeWire API found that both event callbacks had the wrong signatures
  (`process` takes `(void*, struct spa_io_position*)`; `param_changed` takes
  `(void*, void*, uint32_t, const struct spa_pod*)`) and that the RT path
  mixed `pw_filter_get_dsp_buffer()` with the `pw_filter_dequeue_buffer()`
  data layout. Those are corrected, but corrected by reading the API rather
  than by compiling against it. Treat the PipeWire-facing code as unproven
  until it is built and run on a Linux host.
- **WASAPI (Windows) — declared but not implemented.** `wasapi_backend.h`
  exists as a header with no corresponding `.cpp`. `daemon/CMakeLists.txt`'s
  Windows branch used to list that non-existent `wasapi_backend.cpp` as a
  source, so configuring failed outright; it now builds the header-only stub,
  which links (`Open()` logs and returns `false`). The header itself is
  a stub (`Open()` logs and returns `false`) with no real Win32/WASAPI calls
  in it, which is exactly what makes `daemon/tests/test_wasapi_backend.cpp`
  able to build and run cross-platform right now — see §9.
- **CoreAudio (macOS) — not present at all**: neither
  `coreaudio_backend.cpp` nor `.h` exists. The CMake branch that referenced
  them (and the fallback branch referencing an equally absent
  `stub_backend.cpp`) now skips the `eq-daemon` target with an explanatory
  message instead of failing at configure time. The DSP and IPC unit tests
  still build and run on those platforms.

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

Python 3, the room-correction measurement/curve-generation pipeline. Five
modules, each independently testable and now independently tested:

```
measurement.py          flatten.py              export.py           eqapo_export.py       cli.py
──────────────          ──────────              ─────────           ───────────────       ──────
load_wav /               compute_correction:      write_preset /      write_eqapo_config /  measure / eqapo /
load_impulse_response    invert + reference to    read_preset /       render_eqapo_config   plot / send
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

- **`eqapo_export.py`** — **offline, real-world validation path.** Writes an
  [Equalizer APO](https://equalizerapo.com) config file (`Preamp: <dB>` +
  `Filter <n>: ON PK Fc <Hz> Gain <dB> Q <q>` lines, format verified against
  the [official Equalizer APO configuration reference](https://sourceforge.net/p/equalizerapo/wiki/Configuration%20reference/))
  instead of this project's own JSON preset format. Equalizer APO's `PK`
  filter type is the same peaking-biquad shape `DSP::Biquad`/`Equalizer10Band`
  implement (§2.1), so this is a faithful stand-in for this project's own
  filter shape, not a different approximation.

  The reason this exists as a *separate* export path rather than extending
  `export.py`: this project's own audio-hosting code currently has real gaps
  that make it unsuitable for validating the curve-generation math against
  actual playback on Windows — the in-repo APO never actually applies its
  configured curve (§7.1), and the daemon/GUI's Windows IPC path is an
  unimplemented stub on both ends (§7.2), with the daemon not even building
  on Windows (§7.3). Equalizer APO is a mature, independently-verified,
  widely-used engine, so routing a generated curve through it isolates "is
  the curve-generation math right" from "does this project's own APO/daemon
  work" — which is exactly the distinction needed when trying to validate
  correctness in a real room, on real hardware, today.

  `render_eqapo_config()` is a pure string-builder (`gains_db`, `band_hz`,
  `preamp_db`, `q` in → config text out); `write_eqapo_config()` writes it to
  disk. This module only *writes* the format — it does not parse Equalizer
  APO's config files back in, and it has no runtime relationship to this
  project's own DSP; it is generated once and consumed entirely by the
  separate, third-party Equalizer APO application.

- **`cli.py`**: `measure` runs the full pipeline and writes a preset;
  **`eqapo` runs the exact same measurement + correction pipeline** (both
  commands call a shared `_analyse()` helper — deliberately, so `eqapo`
  can't silently drift into testing a different code path than `measure`
  actually ships) **but writes an Equalizer APO config instead**; `plot`
  does the pipeline plus a matplotlib chart (raw vs. smoothed response, bar
  chart of band gains); `send` reads a preset back and pushes it to a
  running daemon over the same Unix socket / JSON-line protocol as the GUI
  (`set_preamp` then `set_bands`), independent of the GUI entirely;
  `visualize` builds the 4-stage validation report described in §6.1.

### 6.1 Visualization tool (`curvegen/visualize.py`, `eq-curvegen visualize`)

Exists to answer one question end-to-end, visually: does the generated
correction curve actually do what the math says it should, once it's
applied to a real room? It renders four panels, each shown two ways:

1. **Recorded input** — the "before" measurement, straight from
   `loaders.load()` (§6.2), smoothed with `measurement.smooth_octave`.
2. **Curve generated** — the continuous EQ response the 10 cascaded
   peaking bands actually produce, evaluated at every point on a synthetic
   log-spaced frequency grid (`synthetic_freq_grid`, 500 points, 20 Hz –
   min(20 kHz, ~Nyquist)) using `evaluate_eq_response_db`, a direct Python
   port of `DSP::Biquad::SetPeaking`'s exact RBJ formula (same
   `omega`/`alpha`/`A` derivation as `DSP/Biquad.cpp`, see its comments) —
   not an approximation, so this panel reflects the same math the daemon
   would apply, independent of whether the daemon or APO actually works
   (§7.1–§7.3). Bands are cascaded by **summing dB**, matching
   `Equalizer10Band::Process()`'s back-to-back chain (linear gains multiply
   ⇒ dB gains add). The orange dots are each band's *own* requested gain at
   its *own* center frequency — exact by construction for an isolated RBJ
   peaking filter — but they are **not** generally equal to the total
   cascaded curve's value at that same frequency, because neighboring
   bands' skirts overlap and add; the plot labels them "Requested band
   gains" rather than implying they land on the blue curve.
3. **Expected output** — stage 1 (interpolated in log-frequency onto
   stage 2's grid) plus stage 2 plus preamp, computed mathematically. This
   is a prediction, not a measurement.
4. **Recorded output** — a second real "after" measurement, if
   `--recorded-output` is supplied. Optional: omitted entirely, this panel
   renders a placeholder and is marked `available=False` on the `Report`
   object, rather than silently faking data. When present, stage 3's curve
   is overlaid on it (dashed) so predicted-vs-actual can be compared by eye.

**FFT vs. CPB**, applied identically to whichever of the four stages has
data: "FFT" is the raw per-bin resolution result as returned by the
loader/evaluator; "CPB" (Constant Percentage Bandwidth, i.e.
fractional-octave smoothing) is the same data run through the existing
`measurement.smooth_octave` at a configurable fraction (`--cpb-fraction`,
default 1/3 octave). Both are plotted overlaid on every panel — CPB doesn't
replace FFT, it's a second, standard-practice view of the same underlying
curve, since a raw FFT trace is usually too jagged to compare against a
smooth EQ curve by eye.

**What this has and hasn't been checked against**: `plot_report()` was
verified to render correctly in this (headless, no display) Linux sandbox
using matplotlib's `Agg` backend against purely synthetic WAVs (pink-noise
bed with an injected resonant peak and a narrow spike, "corrected" by a
second synthetic WAV with that peak attenuated) — see
`CurveGen/tests/test_visualize.py` and `test_cli_visualize.py`. It has
**not** been checked against a real acoustic measurement or a real
Equalizer APO / eq-daemon output; the biquad math itself is a direct
line-for-line port of `DSP/Biquad.cpp`'s formula rather than a
reimplementation from a textbook, which is the strongest guarantee
available here short of that end-to-end hardware test.

### 6.2 Measurement loader registry (`curvegen/loaders.py`)

A small pluggable registry — `register_loader(name, fn, extensions=[...])`
— added so `visualize` (and eventually `measure`/`plot`) can read
measurement files in formats other than this project's WAV-based one
without rearchitecting. `load(path, fmt=None, ...)` resolves a loader by
explicit `fmt`, else by file extension (`detect_format`), else falls back
to `"wav"`. The only loader currently registered is `"wav"`, a thin wrapper
around the existing `measurement.load_wav` / `load_impulse_response`.

**This intentionally implements only the extension point, not a second
format** — the user has measurements in a different, currently-unspecified
format they intend to visualize later; guessing at that format's column
layout/units/headers here would risk silently misreading real data. Adding
it is meant to be small: write a function matching
`loader(path, channel=0, ir=False) -> (freqs, magnitude_db, sample_rate)`
and call `register_loader`. See the module docstring in
`curvegen/loaders.py` for the full contract, including that `sample_rate`
may be `None` for formats that don't carry one.

  Example: `eq-curvegen eqapo --input room.wav --output config.txt --harman`,
  then install [Equalizer APO](https://equalizerapo.com) and either paste
  `config.txt`'s contents into its main `config.txt`, or reference it with
  an `Include: <path>` line.

---

## 7. Known issues and discrepancies

Things worth knowing before trusting a claim about what this system does,
found while writing tests and this document rather than assumed from the
report.

### 7.1 The Windows APO never applied the configured EQ curve (FIXED)

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

**Practical mitigation:** until this is fixed, `eq-curvegen eqapo` (§6) lets
the curve-generation algorithm still be validated on real Windows hardware,
by routing the generated curve through Equalizer APO instead of this
project's own (currently non-functional) APO.

**Fixed.** The EQ is now a single member, `Equalizer::m_eq`, that
`LockForProcess()` prepares and configures and `APOProcess()` runs audio
through. That also closes a second latent bug the function-local `static`
carried: a function-local static is shared by *every* `Equalizer` instance in
the process, so two APO instances (per endpoint or per stream) would have
shared one filter's coefficients and sample history.

Verified on real code rather than by reading:
`ApoProcess_AppliesTheCurveConfiguredByLockForProcess`
(`Equalizer/tests/test_com_exports.cpp`) drives the real path — a minimal
`IAudioMediaType` stub, `LockForProcess()` with a 48 kHz float32 format, then
`APOProcess()` — and asserts the default `BandEqualizer` curve is audible: a
62 Hz tone sits on a +3 dB band and must come out louder than it went in,
while a 1 kHz tone sits on a 0 dB band and must come out at roughly unity.
Against the pre-fix code all four of its assertions fail. Note the test can
only distinguish "curve applied" from "passthrough" because the default curve
is non-flat (a +5/+3/+2 dB "smiley"); if that default is ever flattened, this
test stops discriminating and needs an explicit curve instead.

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

### 7.6 `Equalizer::m_gain` defaulted to `0.0f`, not the `80%` its comment claimed (FIXED)

`Equalizer.h` used to read:

```cpp
float m_gain = 0.0f; // 80% volume
```

The comment said `80%` (i.e. `0.8f`) but the field was initialized to `0.0f`,
and no code path in this repo ever assigned it. Combined with §7.1 (the EQ
stage `APOProcess` actually ran was an unconfigured passthrough), the shipped
APO multiplied every sample by zero and output silence.

**Fixed: the default is now `1.0f`.** Unity, not `0.8f`. `0.8f` would match
the old comment, but it would mean the APO silently attenuates by ~1.9 dB
with no way for the user to see or change it; any level change belongs in the
band curve or an explicit preamp. `ApoProcess_AppliesTheCurveConfiguredByLockForProcess`
(§7.1) pins the audible result.

`ProcessBlock_ZeroGainProducesSilence` (`Equalizer/tests/test_apo_dsp.cpp`)
still exists and still passes — it passes `0.0f` to `ApoDsp::ProcessBlock()`
explicitly, so it tests that function's gain math rather than the
now-changed default.

---

## 8. Build system

| Component | Tool | Entry point |
|---|---|---|
| DSP lib + `eq-daemon` + `WavEqTest` + all new test executables | CMake ≥ 3.20 | root `CMakeLists.txt` → `daemon/CMakeLists.txt` |
| Windows APO DLL | MSBuild / Visual Studio | `Equalizer.sln` |
| GUI | .NET 8 SDK | `GUI/GUI.csproj` |
| CurveGen | pip / setuptools | `CurveGen/pyproject.toml` |

The CMake side has an `EQUALIZER_BUILD_TESTS` option (default `ON`) that adds
test executables (`dsp_tests`, `overlap_add_tests`, `eq_pipeline_tests`,
`band_equalizer_tests`, `apo_dsp_tests`, `eq_state_tests`,
`ipc_server_tests`, `wasapi_backend_tests`) alongside the existing ones:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

`dsp_tests` / `overlap_add_tests` / `eq_pipeline_tests` (DSP core),
`band_equalizer_tests` / `apo_dsp_tests` (the Windows-independent parts of
`Equalizer/`), and `eq_state_tests` / `ipc_server_tests` / `wasapi_backend_tests`
(daemon protocol/state, plus the WASAPI backend stub) all build independently
of whether `libpipewire-0.3-dev` is installed — they were deliberately kept
free of the PipeWire dependency so they still build and run in minimal
environments (including the one this document was written in, which had no
`cmake` binary at all, no PipeWire dev package, and no package-manager write
access; every test in this document was verified with direct
`g++ -std=c++17 -Wall -Wextra` invocations mirroring exactly what the CMake
targets above declare, not assumed from reading the CMake files).

Two more test executables live under `Equalizer/tests/` as standalone Visual
Studio projects (`EqualizerRegistryUtilTests.vcxproj`,
`EqualizerComExportsTests.vcxproj`, both added to `Equalizer.sln`) rather
than in the CMake build, because they need real COM/Win32 registry headers
that don't exist off Windows. Neither could be compiled or run in the
environment that wrote them — build and run them in Visual Studio to
confirm they pass. See §9 for what each one covers.

The `dsp` static library now also compiles `Equalizer/kissfft-131.2.0`'s
`kiss_fft.c`/`kiss_fftr.c` (vendored, BSD-3-Clause) — see §2.3. Fixed in the
same pass: `WavEqTest` (`Tools/WavEqTest.cpp`) uses `BandEqualizer`
(`Equalizer/BandEqualizer.{h,cpp}`), but that source file had never actually
been added to the `WavEqTest` CMake target, so the tool could never link on
a non-Windows build (`undefined reference to BandEqualizer::BandEqualizer()`)
— a pre-existing gap, unrelated to the kissfft wiring, found while
re-verifying the full build after adding it. `BandEqualizer.cpp` is now part
of the `WavEqTest` target.

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
| Measurement loader registry | `CurveGen/tests/test_loaders.py` | Extension auto-detection (incl. uppercase), explicit-format override, `wav` fallback for unrecognised extensions, `--ir` flag routing, unknown-format error message, register/overwrite semantics |
| Visualization report (biquad eval + 4-stage build + render) | `CurveGen/tests/test_visualize.py`, `CurveGen/tests/test_cli_visualize.py` | 0dB-gain exact passthrough, single-band response equals its own gain at its own center frequency (independent of Q), cascaded-band dB additivity, `synthetic_freq_grid` bounds/monotonicity, `build_report` with/without stage 4, expected-output arithmetic vs. a manual recomputation, PNG actually rendered (with and without stage 4) via matplotlib's `Agg` backend, CLI end-to-end against synthetic WAVs |
| FFT block convolution | `DSP/tests/test_overlap_add.cpp` | Parameter validation, `fftSize` always `{2,3,5}`-smooth, latency == `blockSize − 1` exactly, matches direct time-domain convolution (single call and arbitrary non-block-aligned call sizes), multi-channel independence, `Reset()` clears carried tail, filter hot-swap affects only not-yet-computed blocks |
| Equalizer APO config export | `CurveGen/tests/test_eqapo_export.py`, `CurveGen/tests/test_cli_eqapo.py` | Preamp/filter line format round-trips through a regex parser, `PK` filter type used, 1-indexed sequential filter numbers, default vs. custom band list, scalar vs. per-band Q, parameter validation, `eqapo` CLI subcommand end-to-end against a synthetic WAV, and that `eqapo` and `measure` agree on the underlying gains/preamp (shared `_analyse()` pipeline) |
| Combined FIR+IIR pipeline | `DSP/tests/test_eq_pipeline.cpp` | IIR-only/FIR-only/both/neither routing matches the priority table (§2.4) byte-for-byte against standalone `Equalizer10Band`/`OverlapAdd` reference instances, unprepared-instance passthrough, all-zero-gain leaves IIR inactive, oversized-tap rejection, `Reset()` preserves active flags, `Prepare()` resets FIR but preserves IIR (incl. the stale-coefficients-after-rate-change subtlety) |
| APO per-block DSP (gain/EQ/clamp) | `Equalizer/tests/test_apo_dsp.cpp` | Zero-frame no-op, unity-gain passthrough, gain applied before clamping, zero-gain-yields-silence (§7.6), positive/negative overrange clamping, in-place vs. out-of-place equivalence, multi-channel frame-count accounting. Cross-platform (builds and runs without Windows) — this is `ApoDsp::ProcessBlock()`, extracted from `Equalizer::APOProcess()` specifically to make this possible. |
| `BandEqualizer` default curve | `Equalizer/tests/test_band_equalizer.cpp` | Default 10-band count/centers/ordering, gain get/set round-trip, out-of-range index is a no-op (not a crash), band mutation doesn't leak into neighboring bands' center frequency. Cross-platform. |
| WASAPI daemon backend (stub) | `daemon/tests/test_wasapi_backend.cpp` | `Open()` currently returns `false`, `Name()` self-identifies as a stub, `Close()`/destructor are safe with or without a prior `Open()`, the `CreateAudioBackend()` factory returns a working `WasapiBackend`. Cross-platform *only because the backend is still a stub* (§7.3) — must move behind `if(PLATFORM_WINDOWS)` once it grows real WASAPI calls; see the comment at the top of the test file. |
| Registry helpers behind APO registration | `Equalizer/tests/test_registry_util.cpp` (Windows-only, `EqualizerRegistryUtilTests.vcxproj`) | `GuidToString` format, string/DWORD value round-trip, idempotent tree deletion, APO catalog entry register/unregister round-trip — all against a `HKEY_CURRENT_USER` scratch key, never the real `HKEY_LOCAL_MACHINE` paths. Not run in the environment that wrote it (no Windows SDK); needs Visual Studio to confirm. |
| COM entry points + direct `APOProcess()` calls | `Equalizer/tests/test_com_exports.cpp` (Windows-only, `EqualizerComExportsTests.vcxproj`) | `DllGetClassObject` CLSID matching, `IClassFactory::CreateInstance` (aggregation rejection, produces a working `IAudioProcessingObjectRT`), `DllCanUnloadNow` tracks `LockServer` ref-counting, and `APOProcess()` called directly with real `APO_CONNECTION_PROPERTY` structs (gain+clamp behavior including the §7.6 silence case, zero-input-connections no-op, zero-frame-count no-op). Deliberately excludes `DllRegisterServer`/`DllUnregisterServer` themselves (real `HKLM` writes, need admin — see `LOCAL_TEST_GUIDE.md`). Not run in the environment that wrote it; needs Visual Studio to confirm. |

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

- **`Equalizer::LockForProcess()`** — building a fake `IAudioMediaType`/
  `WAVEFORMATEX` to exercise format negotiation would need a much larger COM
  mock surface than the direct-call approach used for `APOProcess()`
  (`test_com_exports.cpp` calls `APOProcess()` directly, bypassing
  `LockForProcess()` entirely — `m_channels` just falls back to its
  default-2 branch). This means the actual `Prepare()`/`SetBandsPeaking()`
  call sequence in `LockForProcess()`, and the real-`WAVEFORMATEX` path, are
  still untested. Reasonable next step if this matters: a minimal
  `IAudioMediaType` stub returning a fixed `WAVEFORMATEX`.
- **`DllRegisterServer` / `DllUnregisterServer`** (`ComExports.cpp`) — call
  through to real `HKEY_LOCAL_MACHINE` writes and need admin rights; the
  registry logic they depend on (`RegistryUtil.cpp`) is unit tested against
  a `HKEY_CURRENT_USER` scratch key instead (`test_registry_util.cpp`), but
  the two entry points themselves are still only exercised manually via
  `regsvr32` per `LOCAL_TEST_GUIDE.md`.
- **`pipewire_backend.cpp`** — requires a running PipeWire graph (or at least
  `libpipewire-0.3-dev` to compile); untested here for the same reason the
  daemon couldn't be built end-to-end in this environment.
- **`coreaudio_backend.h`** — no corresponding `.cpp` exists to test, and
  unlike `wasapi_backend.h` there's no header-only stub to exercise either
  (§7.3).
- **The GUI** (`GUI/`) — no .NET SDK was available in the environment this
  was written in, and there is no existing GUI test project to extend. The
  ViewModel logic (throttling, clamping, preset round-trip) is a reasonable
  next target if a test project is added.
- **A second measurement-file format** — `curvegen/loaders.py` only has the
  extension point implemented (§6.2); no second format is registered or
  tested yet, since its exact layout wasn't known when this was written.
- **`visualize`'s output against a real acoustic measurement or real
  Equalizer APO / eq-daemon audio** — only checked against synthetic WAVs
  in this sandbox (§6.1); the biquad math is a direct port of
  `DSP/Biquad.cpp`, which is the closest available substitute for that
  end-to-end hardware test.
- **`Tools/WavEqTest.cpp`** — this is a manual inspection CLI, not something
  with pass/fail assertions; it's still useful as a quick end-to-end sanity
  check (`WavEqTest in.wav out.wav`) and is exercised that way in
  `LOCAL_TEST_GUIDE.md`-style manual testing, but it isn't part of the
  automated suite. Its FIR+IIR output was, however, independently
  cross-checked outside this repo's own test/toolchain — against from-scratch
  NumPy/SciPy reference implementations (linear convolution, and a
  standalone RBJ-biquad time-domain recursion) run over the real compiled
  binary's WAV output, max error ~1e-4, consistent with float32 precision.
- **`DSP/OverlapAdd.{h,cpp}` integrated into a live signal chain** — covered
  by `DSP/tests/test_overlap_add.cpp` for correctness/RT-safety of the
  engine itself, and by `DSP/tests/test_eq_pipeline.cpp` for its combination
  with `Equalizer10Band` inside `EqPipeline` (§2.4). Still not covered:
  `EqPipeline` running inside the actual PipeWire RT callback (see the
  `pipewire_backend.cpp` bullet above), and it is not wired into the
  Windows APO or WASAPI/CoreAudio backends at all (§2.3, §4.3).
- **Equalizer APO itself** — `eq-curvegen eqapo`'s output has been checked
  against the format Equalizer APO documents and against a hand-rolled test
  parser, but it has not been run through an actual Equalizer APO
  installation in this environment (no Windows available here). Treat the
  generated config as format-correct and pipeline-consistent, not as
  confirmed-working against the real application, until someone does that
  check on real Windows hardware.

---

*This document describes the codebase as of the commit it was added in. If
you fix §7.1–§7.3, please update this file in the same change — that's the
whole point of keeping it next to the code instead of in the thesis PDF.*
