# Test results

Records of executed test runs. The procedure being followed is
[`TEST_PLAN.md`](TEST_PLAN.md); this file records what actually happened when it
was run, including the numbers.

---

## Run 1 — 2026-09-01, Phases 0–3

**Commit:** `6180710` (branch `claude/code-review-2ed8ee`, working tree clean)
**Outcome:** Phases 0–2 pass. **Phase 3 passes** — the correction loop closed and
converged over two iterations on real hardware.

| Metric (above 100 Hz) | Before | After pass 1 | After pass 2 |
|---|---|---|---|
| RMS deviation | 4.19 dB | 1.61 dB | **0.90 dB** |
| Worst band | 8.70 dB | 3.16 dB | **1.78 dB** |

79% reduction in RMS deviation, improving monotonically.

Phase 4 (in-repo APO install) and Phase 5 (Linux daemon) were **not** run.

---

### Environment

| Component | Version |
|---|---|
| OS | Windows 11 |
| Toolchain | Visual Studio 2026 Community, MSVC 14.51 |
| Python | 3.14.3, venv at `.venv` |
| .NET SDK | 10.0.302 |
| Equalizer APO | installed, `C:\Program Files\EqualizerAPO` |
| REW | **not installed** — see "Deviations" below |

### Phases 0–2

| Step | Expected | Actual |
|---|---|---|
| 0.1 Toolchain | cl / msbuild / cmake / ninja resolve | ✅ MSVC 14.51 |
| 0.2 Python venv | `deps ok` | ✅ |
| 0.3 .NET | 8.0+ SDK | ✅ 10.0.302 |
| 1.1 ctest | 7/7 pass | ✅ 7/7 |
| 1.2 APO DLL | 4 configurations build | ✅ all 4 |
| 1.3 RegistryUtilTests | 28 checks, 0 failures | ✅ |
| 1.3 ComExportsTests | 42 checks, 0 failures | ✅ |
| 1.4 pytest | 88 passed, 1 warning | ✅ |
| 1.5 GUI | 0 warnings, 0 errors | ✅ |
| 1.6 eq-daemon | exits 1 on IPC stub | ✅ exit 1 |
| 2.2 Synthetic curve | −9.56 @125 Hz, +4.09 @2 kHz | ✅ every band to the decimal |

Running the plan also exposed two defects **in the plan itself**, fixed in
`6180710`: a broken 0.1 verification command, and the `cmd` parse-time
expansion trap (`%PATH%` / `%errorlevel%` expand when a line is *parsed*, so
one-lining the toolchain activation silently discards it).

---

### Deviations from the plan

Phase 3.1 specifies capturing an impulse response with REW. REW is not
installed on this machine, so **sweep capture and deconvolution were built for
this run** (source in the appendix) rather than using the documented tool.

Two further deviations, both accepted deliberately:

- **Uncalibrated microphone.** The only inputs available are a Realtek laptop
  mic array and the mic input on an iBasso DC04U. The measured curve therefore
  corrects microphone + speaker + room *combined*, and is not a valid listening
  preset. This was accepted because the goal of the run was to validate that
  **successive measurements are consistent and that the loop converges** — for
  which the microphone only needs to be repeatable, not accurate. Repeatability
  was measured, not assumed (see below).
- **Flat target, not Harman.** `--harman` was deliberately omitted so the
  success criterion is unambiguous: a correct loop drives the measured response
  toward 0 dB.

---

### New tooling and its offline validation

An exponential-sine-sweep (Farina) capture and deconvolution path was written
to replace REW. Because this is new code in the trust-critical path — a subtly
wrong deconvolution yields a confidently wrong curve — it was **validated
offline before being pointed at hardware**: synthesise a sweep, push it through
a filter whose response is known in closed form, deconvolve, and compare.

Recovery error against the known cascade, referenced to 1 kHz:

| Frequency | 31 Hz | 62 Hz | 125 Hz | 250 Hz | 500 Hz | 2 kHz | 4 kHz | 8 kHz |
|---|---|---|---|---|---|---|---|---|
| Error | −0.87 | −0.69 | +0.18 | −0.05 | +0.14 | +0.09 | +0.02 | +0.04 |

Max 0.87 dB, mean 0.26 dB, unchanged at −60 and −40 dB SNR; degrading only to
1.05 dB max at −20 dB SNR. The low-frequency error is expected: a
16384-sample IR at 48 kHz is 341 ms, which limits resolution at 31 Hz.

> A first version of the validation script reported a spurious constant
> ~1.85 dB error. The script was wrong, not the deconvolution — it compared a
> *single* filter's response against the *cascade's*. Recorded here because the
> failure looked exactly like a real systematic offset.

---

### Measurement chain setup

| Setting | Value |
|---|---|
| Output | WASAPI device 13, `Głośniki (DC04U)` — iBasso DC04U USB DAC → speakers |
| Input | WASAPI device 15, `Zestaw mikrofonów (Realtek(R) Audio)` |
| Sample rate | 48 kHz |
| Sweep | 20 Hz – 20 kHz, 6 s, 0.5 s lead-in, 1.0 s tail |
| IR length | 16384 samples |

#### Level selection

A first level ladder was run while the house was noisy and produced no workable
setting — the input clipped before reaching usable SNR:

| Playback amp | Input peak | SNR |
|---|---|---|
| 0.30 | 2.368 (clipped) | 20.3 dB |
| 0.15 | 1.004 (clipped) | 14.6 dB |
| 0.06 | 0.448 | 4.7 dB |
| 0.02 | 0.177 | 3.0 dB |

The run was aborted and resumed once the house was quiet, with the ambient
floor down from ≈ −33 dBFS to ≈ −44 dBFS:

| Playback amp | Input peak | SNR |
|---|---|---|
| 0.06 | −6.1 dBFS | 15.3 dB |
| **0.10** | **−2.2 dBFS** | **21.8 dB** |
| 0.13 | −0.9 dBFS | 19.3 dB |

`amp = 0.10` was used for the "before" and pass-1 measurements. Pass 2 used
`amp = 0.35` to offset the extra −4.13 dB of preamp; this is safe because every
comparison is referenced to 1 kHz and so is level-independent.

#### AGC investigation — hypothesis rejected

The flat noise floor across playback levels initially looked like automatic
gain control, which would have invalidated before/after comparison by making
the input path time-varying. Tested directly by recording silence and watching
whether the floor climbed: drift was +4.3 dB over 6 s in one run and −7.9 dB in
another — inconsistent with gain-riding. **The hypothesis was wrong**; the
cause was ordinary household noise. The repeatability result below settles it
conclusively.

---

### Repeatability — the gate for this run

Three identical back-to-back measurements of an unchanged room, `amp = 0.10`,
each processed through the real CurveGen path (`load_impulse_response` →
`smooth_octave` → band interpolation, referenced to 1 kHz):

| Band | run 1 | run 2 | run 3 | spread |
|---|---|---|---|---|
| 31 Hz | −2.39 | −1.39 | −1.69 | 1.00 |
| 62 Hz | +6.85 | +6.78 | +6.84 | 0.07 |
| 125 Hz | +1.40 | +1.38 | +1.47 | 0.09 |
| 250 Hz | +0.28 | +0.31 | +0.33 | 0.06 |
| 500 Hz | −0.31 | −0.39 | −0.33 | 0.07 |
| 2 kHz | −3.94 | −3.96 | −3.96 | 0.02 |
| 4 kHz | −2.94 | −2.92 | −2.92 | 0.02 |
| 8 kHz | +8.70 | +8.69 | +8.71 | 0.02 |
| 16 kHz | −6.18 | −6.26 | −6.18 | 0.08 |

**Above 100 Hz: max spread 0.09 dB, mean 0.04 dB.** Differences between
before and after measurements are therefore signal, not scatter. 31 Hz is the
one soft band (1.00 dB) — little sweep energy and it is where room noise lives;
treat the bottom band as ±1 dB.

---

### Phase 3 results

All values in dB, referenced to 1 kHz. "Before" is the uncorrected room with
Equalizer APO DSP disabled.

| Band | Before | Pass 1 | Pass 2 |
|---|---|---|---|
| 31 Hz | −2.39 | −3.73 | +0.53 |
| 62 Hz | +6.85 | −0.27 | +1.43 |
| 125 Hz | +1.40 | −3.16 | **+0.32** |
| 250 Hz | +0.28 | −1.79 | **+0.77** |
| 500 Hz | −0.31 | −1.18 | −0.01 |
| 1 kHz | 0.00 | 0.00 | 0.00 |
| 2 kHz | −3.94 | −0.38 | −0.87 |
| 4 kHz | −2.94 | −1.16 | −1.78 |
| 8 kHz | **+8.70** | +1.27 | **−0.32** |
| 16 kHz | −6.18 | −1.78 | −1.32 |

Curves applied: pass 1 preamp −5.14 dB, pass 2 preamp −4.13 dB (−9.27 dB
total). Pass 2 was applied *in addition to* pass 1, since the residual was
measured with pass 1 active — both `Include:` lines were live.

Pass-2 repeatability across its two runs: 0.22 dB max above 100 Hz.

The two largest defects — +6.85 dB at 62 Hz and +8.70 dB at 8 kHz — were
reduced to +1.43 and −0.32 dB.

---

### Model check

Does the DSP model predict the physical result? Comparing
`before + cascade₁` (computed with `response.evaluate_eq_response_db()`)
against the measured pass-1 outcome:

**Model error above 100 Hz: max 1.19 dB, mean 0.46 dB.**

The chain behaves as the model says, so the residuals below are attributable to
the solver rather than to modelling or measurement error.

The same model predicted pass 2 at 0.52 dB RMS; the measured result was
0.90 dB (per-band error max 0.92 dB, mean 0.29 dB). Good enough to steer with,
not precise enough to trust as a final answer.

---

### Defect found: cascade overlap → [#3](https://github.com/tarka1939/Equalizer/issues/3)

`flatten.py` solves each band's gain by inverting the measured response *at
that band's centre only*, but the bands are a cascade of overlapping Q=1
peaking filters. Requested gain is not delivered gain:

| Band | Requested | Delivered | Gap |
|---|---|---|---|
| 125 Hz | −1.40 | **−4.72** | **−3.32** |
| 250 Hz | −0.28 | −2.30 | −2.02 |
| 4 kHz | +2.94 | **+0.59** | **−2.35** |
| 16 kHz | +6.18 | +3.78 | −2.40 |

Consequences observed:

- **Flat bands are actively damaged.** 125 Hz went +1.40 → −3.16 and 250 Hz
  +0.28 → −1.79 on pass 1, because the −6.85 dB correction at 62 Hz drags its
  neighbour down.
- **Iterating relocates the error rather than removing it.** 4 kHz went
  −1.16 → −1.78 on pass 2, as the 8 kHz correction shifted and pulled it along.

Iteration is a workaround. The fix is to solve for gains against the cascade
response — a bounded least-squares problem — instead of pointwise inversion.

### Related limitation → [#4](https://github.com/tarka1939/Equalizer/issues/4)

Curve generation is fixed to ten ISO band centres at a shared Q of 1.0. The
export format, JSON schema, and response model already support arbitrary Fc and
per-filter Q; only the solver and CLI do not. Depends on #3, since free filter
placement makes overlap more significant, not less.

---

### What this run does and does not establish

**Established:**

- Sweep capture and Farina deconvolution work (<0.2 dB above 125 Hz offline,
  confirmed on hardware).
- `measurement.py` IR loading is correct on real IRs, including the
  peak-centering fix — real exports have their peak near the start of the file,
  which the previous full-length Hann window destroyed.
- `flatten.py` correction generation and auto-preamp produce a curve that
  measurably flattens a real response.
- `eqapo_export.py` output is consumed by Equalizer APO without complaint and
  applied as written.
- The loop **converges** over successive iterations rather than oscillating.

**Not established:**

- **Absolute acoustic accuracy.** The microphone is uncalibrated, so the curve
  corrects mic + speaker + room together. It is not a listening preset and
  should not be judged by ear.
- Anything about the in-repo Windows APO in a live audio path (Phase 4 not run).
- Anything about the Linux/PipeWire daemon (Phase 5 not runnable).
- Behaviour below ~62 Hz, where measurement uncertainty is ±1 dB.

---

### Reproduction

Phase 3 as run here needs the capture tooling in the appendix, `sounddevice`
(`pip install sounddevice`), and device indices from
`python -c "import sounddevice; print(sounddevice.query_devices())"`.

Sequence: measure with DSP disabled → `eq-curvegen eqapo --input <ir> --ir
--output room_curve.txt` → add `Include: room_curve.txt` to Equalizer APO's
`config.txt` → re-measure → repeat, **adding** each new curve alongside the
previous ones rather than replacing them.

---

## Appendix — capture tooling

Written for this run and not currently part of the repo. Kept here so the
results above are reproducible.

### `sweep.py`

```python
"""Exponential-sine-sweep generation and deconvolution (Farina method)."""
import numpy as np


def make_sweep(f1=20.0, f2=20000.0, dur=6.0, sr=48000, fade=0.05):
    """Farina exponential sine sweep, amplitude 1.0, with raised-cosine fades."""
    n = int(dur * sr)
    t = np.arange(n) / sr
    K = dur * 2 * np.pi * f1 / np.log(f2 / f1)
    L = np.log(f2 / f1) / dur
    x = np.sin(K * (np.exp(t * L) - 1.0))
    nf = max(1, int(fade * sr))
    w = 0.5 * (1 - np.cos(np.pi * np.arange(nf) / nf))
    x[:nf] *= w
    x[-nf:] *= w[::-1]
    return x.astype(np.float64)


def inverse_filter(x, f1=20.0, f2=20000.0, dur=6.0, sr=48000):
    """Time-reversed sweep with a -6 dB/octave envelope, so sweep * inverse -> delta."""
    n = len(x)
    t = np.arange(n) / sr
    L = np.log(f2 / f1) / dur
    return (x[::-1] * np.exp(-t * L)).astype(np.float64)


def deconvolve(recorded, inv, sr=48000, ir_len=16384):
    """Recover the impulse response, keeping ir_len samples around the peak."""
    n = 1
    while n < len(recorded) + len(inv):
        n *= 2
    full = np.fft.irfft(np.fft.rfft(recorded, n) * np.fft.rfft(inv, n), n)
    peak = int(np.argmax(np.abs(full)))
    pre = min(peak, 256)
    ir = full[peak - pre: peak - pre + ir_len]
    if len(ir) < ir_len:
        ir = np.pad(ir, (0, ir_len - len(ir)))
    m = np.max(np.abs(ir))
    return (ir / m if m > 0 else ir), peak
```

### `capture.py`

```python
"""Play a log sweep, record it, deconvolve to an impulse response."""
import sys, numpy as np, sounddevice as sd, scipy.io.wavfile as wav
from sweep import make_sweep, inverse_filter, deconvolve

SR, DUR, F1, F2 = 48000, 6.0, 20.0, 20000.0
OUT_DEV, IN_DEV = 13, 15          # set from sounddevice.query_devices()

def measure(out_wav, amp=0.10, pre=0.5, post=1.0):
    x = make_sweep(F1, F2, DUR, SR)
    sig = np.concatenate([np.zeros(int(pre*SR)), x*amp, np.zeros(int(post*SR))])
    play = np.column_stack([sig, sig]).astype(np.float32)

    rec = sd.playrec(play, samplerate=SR, channels=1,
                     device=(IN_DEV, OUT_DEV), dtype='float32', blocking=True)
    r = rec[:, 0].astype(np.float64)

    peak = float(np.max(np.abs(r)))
    noise = float(np.sqrt(np.mean(r[:int(pre*SR*0.8)]**2))) + 1e-12
    rms   = float(np.sqrt(np.mean(r[int(pre*SR):int((pre+DUR)*SR)]**2)))

    ir, _ = deconvolve(r, inverse_filter(x, F1, F2, DUR, SR), SR)
    wav.write(out_wav, SR, ir.astype(np.float32))
    print(f"{out_wav}: peak {20*np.log10(peak+1e-12):+.1f} dBFS, "
          f"SNR {20*np.log10(rms/noise):.1f} dB"
          + ("   *** CLIPPING ***" if peak >= 0.999 else ""))
    return ir

if __name__ == "__main__":
    measure(sys.argv[1], amp=float(sys.argv[2]) if len(sys.argv) > 2 else 0.10)
```

Aim for an input peak near −6 dBFS with no clipping. If no playback level
achieves both adequate peak and SNR, the ambient noise floor is too high —
measure when it is quieter rather than pushing the level.
