# Test plan

End-to-end verification plan for System Equalizer, from a cold checkout through
a real acoustic room-correction loop.

Every command and every expected result below was executed on Windows 11 with
Visual Studio 2026 Community (MSVC 14.51) at merge commit `72a146eb`. Numbers
quoted as expected output are **measured**, not estimated.

Related documents:

- [`LOCAL_TEST_GUIDE.md`](LOCAL_TEST_GUIDE.md) — the manual APO install /
  register / verify / uninstall procedure referenced by Phase 4.
- [`ARCHITECTURE.md`](ARCHITECTURE.md) §7 — known gaps, cross-referenced below.
- [`README.md`](README.md) — build overview.
- [`TEST_RESULTS.md`](TEST_RESULTS.md) — what actually happened when this
  plan was executed, including the Phase 3 numbers.

---

## Scope

| Phase | What it proves | Hardware needed | Time |
|---|---|---|---|
| 0 | Toolchain is usable | — | 5 min |
| 1 | Everything builds and all unit tests pass | — | 10 min |
| 2 | Curve-generation math is correct (deterministic) | — | 5 min |
| 3 | **Real room correction works, end to end** | Mic + speakers | 45 min |
| 4 | The in-repo Windows APO loads and processes audio | Windows audio endpoint | 30 min |
| 5 | Linux daemon path | Linux + PipeWire | not runnable today |

Phases 0–2 need no hardware and should pass 100%. Phase 3 is the real
deliverable. Phase 4 is optional and mutates machine-global registry state.

### Not covered, and why

| Area | Status |
|---|---|
| `daemon/pipewire_backend.cpp` | Never compiled — no `libpipewire-0.3-dev` on any machine used so far. Unproven. |
| Windows daemon audio | `wasapi_backend.h` is a stub; `Open()` logs and returns `false`. |
| Windows IPC (daemon ↔ GUI) | Unimplemented on both ends (`ARCHITECTURE.md` §7.2). The GUI's **Connect** button cannot succeed on Windows — this is expected, not a defect. |
| FIR / convolution path | `set_fir` exists in the daemon, but nothing in CurveGen or the GUI generates taps, so it has no producer. |
| Sweep deconvolution | Not implemented. See the warning in Phase 3. |

---

## Phase 0 — Environment

### 0.1 Activate the MSVC toolchain

None of MSVC, CMake, or Ninja are on `PATH`. Every C++ step below assumes a
shell prepared like this. Run from a **`cmd`** prompt at the repo root:

```cmd
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
```

> ⚠️ **Enter those as two separate lines.** Do not join them with `&` on one
> line, and do not wrap them in `cmd /c "... & ..."`. `cmd` expands `%PATH%`
> when it *parses* the line, not when it runs it, so a one-liner substitutes
> the PATH from **before** `vcvars64.bat` ran and silently discards everything
> the batch file added — `cl` and `msbuild` then aren't found. The same
> parse-time trap applies to `%errorlevel%`: `somecommand & echo %errorlevel%`
> reports the code from the *previous* command. Use separate lines, or a `.bat`
> file (which is parsed line by line).

**Expected:** `cl`, `msbuild`, `cmake`, and `ninja` all resolve.

```cmd
where cl & where msbuild & where cmake & where ninja
```

Verified output paths: `...\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\cl.exe`,
`...\MSBuild\Current\Bin\amd64\MSBuild.exe`, and the two bundled
`...\CommonExtensions\Microsoft\CMake\...` entries. `INFO: Could not find files
for the given pattern(s)` for `cl` or `msbuild` means the toolchain did not
activate — re-read the warning above.

> For 32-bit builds use `vcvars32.bat` instead. A 32-bit APO cannot load in the
> x64 audio engine, so Win32 is a build-health check only, never a shipping
> target.

### 0.2 Create the Python environment

```cmd
python -m venv .venv && .venv\Scripts\python.exe -m pip install -q -e "CurveGen[dev]"
```

**Expected:** exit code 0. Confirm:

```cmd
.venv\Scripts\python.exe -c "import numpy,scipy,matplotlib,jsonschema,pytest; print('deps ok')"
```

**Expected:** `deps ok`

### 0.3 .NET

```cmd
dotnet --version
```

**Expected:** an 8.0 or later SDK. The GUI targets `net8.0`; a newer SDK rolls
forward fine (verified on 10.0.302).

---

## Phase 1 — Build and unit-test gate

Every step here must pass before touching hardware. A failure means stop and
fix; do not proceed to Phase 3.

### 1.1 Cross-platform CMake build + ctest

```cmd
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build --output-on-failure
```

**Expected:**

```
100% tests passed, 0 tests failed out of 7
```

The 7 are `dsp_tests`, `overlap_add_tests`, `eq_pipeline_tests`,
`band_equalizer_tests`, `apo_dsp_tests`, `eq_state_tests`,
`wasapi_backend_tests`.

> `ipc_server_tests` is **deliberately absent on Windows** — it drives a real
> Unix domain socket and is guarded by `if(NOT PLATFORM_WINDOWS)`. On Linux you
> should see 8 tests, not 7.

### 1.2 APO DLL — all four configurations

```cmd
for %C in (Release Debug) do for %P in (x64 Win32) do msbuild Equalizer\Equalizer.vcxproj /p:Configuration=%C /p:Platform=%P /v:minimal /nologo
```

**Expected:** four `Equalizer.vcxproj -> ...\Equalizer.dll` lines, no errors.

> Two historical traps guard this step. Headers must stay in `<ClInclude>` — a
> header in `<ClCompile>` produced an `Equalizer.obj` that collided with the
> real one and made the link fail **intermittently**. And both `.vcxproj` files
> need `<LanguageStandard>stdcpp17</LanguageStandard>` plus `runtimeobject.lib`.
> See the gotchas in [`CLAUDE.md`](CLAUDE.md).

### 1.3 Windows-only test suites

```cmd
msbuild Equalizer\tests\EqualizerRegistryUtilTests.vcxproj /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo && msbuild Equalizer\tests\EqualizerComExportsTests.vcxproj /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
```

Then run both:

```cmd
Equalizer\tests\x64\Release\EqualizerRegistryUtilTests.exe & Equalizer\tests\x64\Release\EqualizerComExportsTests.exe
```

**Expected:**

```
28 checks, 0 failures
42 checks, 0 failures
```

Both exit 0. `RegistryUtilTests` writes only to an `HKEY_CURRENT_USER` scratch
key and needs no admin rights.

The check that matters most here is
`ApoProcess_AppliesTheCurveConfiguredByLockForProcess` — it drives
`LockForProcess()` → `APOProcess()` and asserts the configured curve is
actually audible. If that one fails, the APO is not applying EQ and Phase 4 is
pointless.

### 1.4 Python suite

```cmd
.venv\Scripts\python.exe -m pytest CurveGen/tests -q
```

**Expected:**

```
88 passed, 1 warning
```

The single warning is expected: `test_output_length_matches_bands` deliberately
requests a 20 Hz band from a measurement that starts at 23.4 Hz, exercising the
out-of-range warning. A warning here is the feature working.

### 1.5 GUI

```cmd
dotnet build GUI/GUI.csproj -c Release --nologo
```

**Expected:** `0 Warning(s)`, `0 Error(s)`.

### 1.6 Daemon smoke test (Windows)

```cmd
build\daemon\eq-daemon.exe --no-audio
```

**Expected — this "failure" is the correct result:**

```
eq-daemon v1.0 starting
[IPC] Windows named pipe not yet implemented.
Failed to start IPC server — aborting.
```

Exit code 1. It proves the binary links and starts; the Windows IPC path is a
known stub (`ARCHITECTURE.md` §7.2).

---

## Phase 2 — CurveGen self-test with a known answer

Validates the measurement → correction math against a synthetic room whose
response is known in closed form. No microphone required. **Run this before
every real measurement session** — it takes seconds and catches a broken
install immediately.

> Work in a scratch directory outside the repo, or expect `make_test_ir.py` to
> show up as untracked. `*.wav` and `build/` are gitignored; a stray `.py` at
> the repo root is not. If you run from elsewhere, invoke the venv by absolute
> path instead of the relative `.venv\Scripts\...` shown below.

### 2.1 Generate a synthetic impulse response

Save as `make_test_ir.py`:

```python
"""Synthetic IR with a KNOWN response: +8 dB resonance at 125 Hz, -6 dB dip at 2 kHz."""
import numpy as np, scipy.signal as sg, scipy.io.wavfile as wav

SR, N = 48000, 16384

def peaking(fc, q, gain_db, sr=SR):
    A = 10 ** (gain_db / 40); w = 2 * np.pi * fc / sr
    al = np.sin(w) / (2 * q); c = np.cos(w)
    b = np.array([1 + al * A, -2 * c, 1 - al * A])
    a = np.array([1 + al / A, -2 * c, 1 - al / A])
    return b / a[0], a / a[0]

ir = np.zeros(N); ir[0] = 1.0
for fc, q, g in [(125.0, 1.0, 8.0), (2000.0, 1.0, -6.0)]:
    b, a = peaking(fc, q, g)
    ir = sg.lfilter(b, a, ir)

ir = np.concatenate([np.zeros(120), ir])[:N]   # peak ~2.5 ms in, like a real export
ir /= np.max(np.abs(ir))
wav.write("synthetic_ir.wav", SR, ir.astype(np.float32))
print("wrote synthetic_ir.wav")
```

```cmd
.venv\Scripts\python.exe make_test_ir.py
```

### 2.2 Run the pipeline against it

```cmd
.venv\Scripts\eq-curvegen.exe eqapo --input synthetic_ir.wav --ir --output synthetic_config.txt
```

**Expected — these are measured values, tolerance ±0.3 dB:**

```
 Band (Hz)  Correction (dB)
 ─────────  ───────────────
      31    -1.57
      62    -0.92
     125    -9.01      <-- inverts the +8 dB room resonance
     250    -0.98
     500    -1.00
    1000    -0.90
    2000    +4.84      <-- fills the -6 dB room dip
    4000    -0.97
    8000    -1.30
   16000    -1.99

 Preamp:    -4.07 dB
```

**Pass criteria:**

- 125 Hz is a large **negative** correction (≈ −9 dB) — the room's boost is being cut.
- 2 kHz is a **positive** correction (≈ +4.8 dB) — the room's dip is being filled.
- Every other band sits near **−1 dB**, all within a few tenths of each other.
- Preamp is negative and roughly the cascade's worst-case boost.

**Reading the flat bands.** The room is flat everywhere except 125 Hz and
2 kHz, so those eight bands "should" read 0.00. They read ≈ −1 dB instead, and
that is correct: the 2 kHz dip's skirt reaches down to 1 kHz, so the reference
point itself sits ≈ 0.9 dB low, and every band is expressed relative to it. The
result is a broadband −0.9 dB offset — a constant the preamp and your volume
knob absorb. What matters is that the eight flat bands are *equal to each
other*; a spread among them is the cascade-overlap defect (issue #3).

> **These numbers changed when the cascade solver landed**, and the old ones
> are a useful contrast. Pointwise inversion produced −2.35 / −4.28 / −9.56 /
> −4.21 / −1.92 / 0.00 / +4.09 / +0.05 / −1.41 / −1.72: the flat bands scatter
> across 4.3 dB instead of sitting together, because each band's gain was
> chosen without asking what its neighbours were already doing. Residual error
> against the known room fell from 2.27 dB RMS to 0.11 dB.

- 1 kHz no longer reads exactly 0.00. That is expected and is not a
  regression: with overlapping bands the gain *value* at the reference band is
  not the response *delivered* there, and the solver targets the latter. The
  underlying limitation is unchanged — a defect located exactly at 1 kHz is
  still invisible, because everything is measured relative to it
  (`ARCHITECTURE.md` §6/§7.4).

**If 125 Hz comes back near zero or with the wrong sign**, the IR loader is
mangling the input — that was a real bug (a full-length Hann window destroyed
IRs whose peak sat near the start of the file, off by up to 13.7 dB). Re-run
Phase 1.4.

### 2.3 Confirm the emitted config

```cmd
type synthetic_config.txt
```

**Expected:** a `Preamp: -4.07 dB` line followed by ten
`Filter N: ON PK Fc <hz> Hz Gain <db> dB Q 1.00` lines whose gains match the
table above.

---

## Phase 3 — Real-world acoustic loop (the actual deliverable)

This is the primary path. It routes a generated curve through **Equalizer APO**
(the mature third-party engine, <https://equalizerapo.com>), which isolates
"is our curve-generation math right?" from "does our own APO work?".

> ### Read this before measuring
>
> **The `curvegen` package has no capture tooling and does not deconvolve.**
> It only *reads* WAV files. What you feed it matters enormously:
>
> | Input | Use | Result |
> |---|---|---|
> | Impulse response from REW, via `--ir` | ✅ **Do this** | Correct |
> | Impulse response from `CurveGen/tools/capture.py`, via `--ir` | ✅ **Or this** | Correct — validated offline to ≤0.35 dB above 125 Hz |
> | White noise recording, no `--ir` | ⚠️ Acceptable | Correct but a poor stimulus |
> | **Pink noise recording, no `--ir`** | ❌ **Never** | Silently wrong — pink noise falls at −3 dB/octave by definition, which the pipeline "corrects" into a +3 dB/octave boost. Audibly far too bright, and nothing detects it. |
>
> Neither loader divides out the excitation signal — deconvolution has to happen
> before CurveGen sees the file, which is what REW and `capture.py` each do. See
> the warning block at the top of `CurveGen/curvegen/measurement.py`.

### 3.1 Capture the "before" measurement

Either tool below produces an acceptable impulse response. **Option B is what
the run recorded in `TEST_RESULTS.md` actually used**, because REW was not
installed on that machine.

#### Option A — [REW](https://www.roomeqwizard.com/)

1. Calibrate the mic and set levels.
2. Run a swept-sine measurement at the listening position.
3. **Export the impulse response as WAV** (not the frequency response):
   `File → Export → Export impulse response as WAV`. Save as `room_before.wav`.

#### Option B — in-repo sweep capture

```cmd
pip install sounddevice
python -c "import sounddevice; print(sounddevice.query_devices())"
```

Set `OUT_DEV` / `IN_DEV` at the top of `CurveGen/tools/capture.py` from that
listing, then, **from that directory** (`capture.py` imports `sweep` as a
sibling):

```cmd
python capture.py room_before.wav 0.10
```

The second argument is playback amplitude. `capture.py` prints input peak and
SNR per take and flags clipping — aim for a peak near −6 dBFS. Run a short
ladder (0.06 / 0.10 / 0.13) and keep the best. If nothing gives both adequate
peak and SNR, the room is too noisy; measure later rather than pushing the
level. See [`CurveGen/tools/README.md`](CurveGen/tools/README.md).

**Expected (either option):** a WAV whose peak is clearly visible near the
start. Any peak position is fine — the loader centres it internally before
windowing.

> **Before trusting before/after comparisons, measure repeatability.** Take
> three back-to-back measurements of an unchanged room and compare them
> band-by-band. In the recorded run the spread above 100 Hz was 0.04 dB mean /
> 0.09 dB max, which is what makes a later before/after difference signal
> rather than scatter. An uncalibrated mic is fine for this — it only has to be
> repeatable, not accurate — but see the caveat in §3.3.

### 3.2 Generate the correction curve

```cmd
.venv\Scripts\eq-curvegen.exe eqapo --input room_before.wav --ir --harman --output room_curve.txt
```

**Expected:** a per-band table plus `room_curve.txt`. Sanity-check it before
listening:

- Corrections should mostly fall within ±12 dB (the clip limit). **Many bands
  pinned at exactly ±12.00 means something is wrong** — most likely a non-IR
  input, or an IR that wasn't deconvolved.
- The 1 kHz band will read ≈ 0.00 dB. Expected, not a bug.
- If you see `UserWarning: Band centre(s) ... lie outside the measured range`,
  your measurement doesn't span those bands; their values are extrapolated and
  should not be trusted.

Add `--max-gain 6` for a gentler first attempt, and `--q <value>` if you intend
to change Q — it feeds both the solved gains and the headroom calculation, so
they stay consistent.

#### Optional: parametric mode

The command above fits ten filters to the fixed ISO band centres at one shared
Q. `--mode parametric` lets the solver choose each filter's centre frequency
and Q instead:

```cmd
.venv\Scripts\eq-curvegen.exe eqapo --input room_before.wav --ir --mode parametric --output room_curve.txt
```

**Expected:** a table of filters — usually *fewer* than ten, each with its own
Fc and Q — and a config whose `Filter` lines carry differing `Q` values. This
fits real rooms considerably better, especially narrow low-frequency modes,
which the fixed grid cannot place a filter on at all.

**It only works through Equalizer APO.** The JSON preset schema and this
project's own `DSP::Equalizer10Band` are both fixed at ten bands with a single
shared Q, so `measure` has no parametric mode and a parametric curve cannot be
loaded into the GUI or the daemon. See `ARCHITECTURE.md` §6.

### 3.3 Apply it

1. Install Equalizer APO and select your output device in its Configurator.
2. Add the generated file to `config.txt` — prefer an `Include` over pasting, so
   regeneration doesn't need re-editing:

   ```
   Include: C:\path\to\room_curve.txt
   ```

3. Equalizer APO applies changes on save; no reboot needed.

**Expected:** audible change on music, weighted toward whatever your room was
doing wrong. No clipping or distortion on loud passages — the emitted `Preamp:`
line exists to prevent exactly that.

> If you hear clipping, the auto-preamp bound is the cascade's magnitude
> response, which is tight rather than guaranteed (one synthetic case measured
> 0.02 dB over). Lower the `Preamp:` value by another 1–2 dB.

### 3.4 Re-measure and verify convergence

Repeat 3.1 with the correction **active**, saving as `room_after.wav`. Then:

```cmd
.venv\Scripts\eq-curvegen.exe visualize --input room_before.wav --recorded-output room_after.wav --ir --output validation_report.png
```

**Expected:** a 4-panel PNG — recorded input, curve generated, expected output,
recorded output — each with FFT and 1/3-octave views.

**Pass criteria:** panel 4 (recorded output) is measurably flatter than panel 1
(recorded input) across the corrected bands, and broadly resembles panel 3
(mathematically expected output).

**This is the real success condition for the whole project.** Panel 3 vs panel 4
diverging means the math and the physical result disagree — investigate before
trusting any curve.

### 3.5 Iterate

Room correction is iterative. Feed `room_after.wav` back through 3.2 to generate
a residual correction, or widen `--max-gain` once you trust the loop. Expect
diminishing returns after two or three passes; deep narrow nulls cannot be fixed
by EQ at all and trying wastes headroom.

---

## Phase 4 — In-repo Windows APO (optional)

Only meaningful now that the APO actually applies its curve — historically it
could not (two separate `static` EQ objects plus a zero default gain,
`ARCHITECTURE.md` §7.1/§7.6, both fixed).

> ⚠️ This mutates machine-global `HKEY_LOCAL_MACHINE` state and needs admin
> rights. Follow [`LOCAL_TEST_GUIDE.md`](LOCAL_TEST_GUIDE.md) exactly, including
> its registry backup step, and note the CLSID migration warning if you ever
> registered an older build.
>
> Do **not** run Phase 4 concurrently with Equalizer APO on the same endpoint.

### 4.1 Preconditions

Phase 1.2 and 1.3 must be green — especially
`ApoProcess_AppliesTheCurveConfiguredByLockForProcess`.

### 4.2 Install, verify, uninstall

Follow `LOCAL_TEST_GUIDE.md` §0 (backups) → §1 (register) → §2 (enable on
endpoint) → §3 (verify loaded) → §4/§5 (disable and uninstall).

**Expected at §3.1:** `Equalizer.dll` appears in `audiodg.exe`'s loaded modules.

**Expected audibly:** the built-in default curve is a "smiley" — +5 dB at
31 Hz, +3 at 62, +2 at 125, flat mids, +2 at 4 kHz, +3 at 8 kHz, +5 at 16 kHz.
Bass and treble should lift noticeably versus bypass.

**If audio goes silent**, unlock the endpoint and restore per §4 immediately.
There is no IPC to the APO on Windows, so the curve is compiled-in — it is not
adjustable at runtime.

### 4.3 Known limitations

- The APO's curve is fixed at build time (`BandEqualizer`'s constructor). There
  is no way to load a CurveGen preset into it yet.
- File tracing is compiled out by default. Rebuild with
  `EQUALIZER_ENABLE_FILE_LOG` if you need `eq_apo.txt`; `LOCAL_TEST_GUIDE.md`
  §3.2 assumes it is on.

---

## Phase 5 — Linux daemon (not runnable today)

Blocked: `daemon/pipewire_backend.cpp` has never been compiled, and beyond that
the filter node is created with no `node.autoconnect` and no link management, so
nothing routes system audio through it. Expect to need manual `pw-link` wiring
or a null-sink setup even once it compiles.

When a Linux box is available:

```bash
sudo apt install cmake libpipewire-0.3-dev && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure
```

**Expected:** 8 tests (7 plus `ipc_server_tests`, which builds only on POSIX).

---

## Troubleshooting

| Symptom | Cause | Action |
|---|---|---|
| `'cl' is not recognized` | vcvars not sourced | Phase 0.1 |
| `error C7525: inline variables require /std:c++17` | `.vcxproj` missing `<LanguageStandard>` | Add `stdcpp17` to all four configs |
| `LNK2001: RoOriginateError` | Missing `runtimeobject.lib` | Add to `AdditionalDependencies` |
| `LNK2001: CLSID_Equalizer`, intermittent | A header in `<ClCompile>` | Move to `<ClInclude>` |
| `'sys/socket.h': No such file` | `ipc_server_tests` unguarded | Should be `if(NOT PLATFORM_WINDOWS)` |
| Every band pinned at ±12.00 dB | Non-IR input, or missing `--ir` | Re-export an IR from REW |
| Correction sounds far too bright | Pink noise fed to `load_wav` | Use an IR with `--ir` |
| 1 kHz always reads 0.00 dB | By construction | Not a bug — §7.4 |
| GUI "Connect" fails on Windows | IPC is a stub on both ends | Expected — §7.2 |

---

## Sign-off checklist

```
[ ] 0.1  MSVC toolchain activates
[ ] 0.2  Python venv installs
[ ] 1.1  ctest .................. 7/7 pass (8 on Linux)
[ ] 1.2  Equalizer.dll .......... all 4 configurations build
[ ] 1.3  RegistryUtilTests ...... 28 checks, 0 failures
[ ] 1.3  ComExportsTests ........ 42 checks, 0 failures
[ ] 1.4  pytest ................. 88 passed, 1 expected warning
[ ] 1.5  GUI .................... 0 warnings, 0 errors
[ ] 1.6  eq-daemon ............ starts, exits 1 on the IPC stub
[ ] 2.2  Synthetic curve ........ 125 Hz ≈ -9.6, 2 kHz ≈ +4.1
[ ] 3.1  "Before" IR captured from REW
[ ] 3.2  Curve generated, no bands pinned at the clip limit
[ ] 3.3  Curve applied in Equalizer APO, no clipping
[ ] 3.4  "After" measurement is flatter, and matches the expected panel
[ ] 4.x  (optional) In-repo APO loads in audiodg and is audible
```
