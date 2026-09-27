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

The second argument is playback amplitude.

### Measuring one speaker at a time

A correction curve is per-speaker, so the sweep has to come out of one speaker
alone. Playing it through both at once measures their *sum* at the microphone,
where the two arrivals comb-filter against each other — a result that describes
neither speaker.

```bash
python capture.py room_L.wav --channel L      # one speaker
python capture.py --channels L,R --prefix room  # both, in sequence
```

The second form prompts between takes and writes `room_L.wav` and
`room_R.wav`, which is exactly what

```bash
eq-curvegen eqapo --channel-input L=room_L.wav --channel-input R=room_R.wav --ir --output curve.txt
```

consumes. `--channel` accepts `L`, `R`, `C`, `LFE`, `RL`, `RR`, `SL`, `SR`, a
1-based number, or `all`; `--out-channels` sets how many output channels the
device expects if it is not stereo.

Only the playback routing knows about channels — `sweep.py`'s generation and
deconvolution are untouched, so the validation below still covers the part that
matters. Aim for an input peak near
−6 dBFS with no clipping; `capture.py` prints peak and SNR after each take and
flags clipping. If no level gives both adequate peak and SNR, the ambient noise
floor is too high — measure when it is quieter rather than pushing the level.

Run from this directory: `capture.py` imports `sweep` as a sibling module.

The resulting WAV is an impulse response, so pass `--ir`:

```bash
eq-curvegen eqapo --input room_before.wav --ir --output room_curve.txt
```

### Never analyse the raw sweep or its recording

`capture.py` writes the deconvolved impulse response, which is the only thing
CurveGen should see. **A raw sweep — or a recording of one — is not a valid
input, with or without `--ir`.**

A log sweep is *pink*, not flat. Its time-domain amplitude is constant (every
frequency is played at the same level, peak 1.0 throughout), but it spends
equal *time* per octave and therefore deposits equal *energy* per octave — and
each octave up is twice as wide in hertz, so energy per hertz halves. Measured
on this sweep: every octave from 31 Hz to 16 kHz carries the same energy to
within 0.01 dB, while the spectrum falls at **−3.02 dB/octave** (theory:
−3.01).

That slope is the whole point of the design: constant drive level protects the
amplifier's headroom, while equal energy per octave puts the most energy at low
frequencies, where room noise is worst and SNR is hardest to get. The
`inverse_filter` rises at exactly +3.01 dB/octave to cancel it — the product of
the two is flat to −0.00 dB/decade, which is why deconvolution yields a delta.

But CurveGen has no way to distinguish a sloped *stimulus* from a sloped
*room*. Analyse a raw sweep and it will confidently invert the sweep's own
design into the curve: −12.00 dB at 31 Hz, +12.00 dB at 16 kHz, two bands
pinned at the ±12 dB clip limit. See the input table in
[`TEST_PLAN.md`](../../TEST_PLAN.md) §3.

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
