# Measurement capture tooling

Exponential-sine-sweep (Farina) capture and deconvolution: play a log sweep,
record it, and recover an impulse response that `eq-curvegen ... --ir` can read.

These exist because [REW](https://www.roomeqwizard.com/) is the tool
`TEST_PLAN.md` §3.1 assumes, and it is not installed on every machine used on
this project. They were written for the Phase 3 run recorded in
[`TEST_RESULTS.md`](../../TEST_RESULTS.md) and are kept here so that run is
reproducible.

**They are deliberately *not* part of the `curvegen` package.** `curvegen`
reads measurement files; it does not touch audio hardware, and it has no
`sounddevice` dependency. Keeping capture separate preserves that split — the
pipeline stays testable without a sound card.

## Requirements

`sweep.py` needs only `numpy`. `capture.py` additionally needs `scipy` and
`sounddevice`, which is **not** a declared dependency of `eq-curvegen`:

```bash
pip install sounddevice
```

## Use

Find your device indices, set `OUT_DEV` / `IN_DEV` at the top of `capture.py`,
then measure:

```bash
python -c "import sounddevice; print(sounddevice.query_devices())"
python capture.py room_before.wav 0.10
```

The second argument is playback amplitude. Aim for an input peak near
−6 dBFS with no clipping; `capture.py` prints peak and SNR after each take and
flags clipping. If no level gives both adequate peak and SNR, the ambient noise
floor is too high — measure when it is quieter rather than pushing the level.

Run from this directory: `capture.py` imports `sweep` as a sibling module.

The resulting WAV is an impulse response, so pass `--ir`:

```bash
eq-curvegen eqapo --input room_before.wav --ir --output room_curve.txt
```

## What has been verified

`sweep.py`'s deconvolution is validated offline — synthesise a sweep, push it
through a biquad cascade whose response is known in closed form, deconvolve,
and compare against `curvegen.response.evaluate_eq_response_db`. Recovery
error, referenced to 1 kHz:

| | 31 Hz | 62 Hz | ≥ 125 Hz |
|---|---|---|---|
| Error | −1.5 dB | −1.3 dB | ≤ 0.35 dB |

Unchanged down to −20 dB SNR. The low-frequency error is expected rather than a
defect: a 16384-sample IR at 48 kHz is 341 ms, which limits resolution at 31 Hz.
Treat the bottom two bands as ±1.5 dB.

`capture.py` cannot be validated without hardware; it is the sweep/record/write
wrapper around the part that can be.

> **Two traps this check walked into, both worth knowing.**
>
> Compare against the **cascade's** response, not a single filter's — that
> mistake produced a spurious constant ~1.85 dB error that looked exactly like
> a real systematic offset.
>
> Centre the IR's peak before windowing. A full-length Hann window is ~0 at
> index 0, and a deconvolved IR has its peak near the start, so windowing in
> place multiplies the direct sound away: re-running this check without the
> centring step reports 19 dB of error at 31 Hz and 4.4 dB at 125 Hz.
> `measurement.load_impulse_response` already does this correctly — any
> harness that reimplements the FFT path has to do it too.
