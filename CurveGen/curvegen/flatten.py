"""
flatten.py — Compute a correction EQ curve that "flattens" a measured
             frequency response.

Algorithm
---------
1. Interpolate the measured response at each EQ band centre frequency.
2. Invert: correction_gain = -measured_gain (at that frequency).
3. Optionally blend with a psychoacoustic target curve (Harman 2018).
4. Apply safety clipping to ±MAX_GAIN_DB.
5. Optionally compute a preamp headroom correction so the loudest band
   does not clip.
"""
from __future__ import annotations

import warnings

import numpy as np
from typing import Optional, Sequence, Tuple

from .response import DEFAULT_SAMPLE_RATE, cascade_peak_db

# ── Constants ─────────────────────────────────────────────────────────────────

DEFAULT_BAND_HZ: list[float] = [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]
MAX_GAIN_DB: float = 12.0    # hard per-band limit
MIN_FREQ_HZ: float = 20.0    # ignore below this (unreliable measurements)

# Harman 2018 in-room target (deviation from flat in dB, referenced to 1 kHz).
# Values at: 20, 40, 80, 160, 315, 630, 1250, 2500, 5000, 10000, 20000 Hz
_HARMAN_HZ  = np.array([20,  40,  80, 160, 315, 630, 1250, 2500, 5000, 10000, 20000], dtype=float)
_HARMAN_DB  = np.array([3.0, 4.0, 3.5, 2.0, 1.0, 0.5,  0.0, -1.0, -3.0, -5.0,  -9.0])


# ── Public API ────────────────────────────────────────────────────────────────

def compute_correction(
    freqs: np.ndarray,
    magnitude_db: np.ndarray,
    band_hz: Optional[Sequence[float]] = None,
    max_gain_db: float = MAX_GAIN_DB,
    use_harman_target: bool = False,
    harman_blend: float = 0.5,
    auto_preamp: bool = True,
    q: float = 1.0,
    sample_rate: float = DEFAULT_SAMPLE_RATE,
) -> Tuple[np.ndarray, float]:
    """
    Compute per-band correction gains that flatten the measured response.

    Parameters
    ----------
    freqs, magnitude_db  : measured frequency response (from measurement.py).
                           Note measurement.py's warning: these are raw
                           spectra of the recording, not deconvolved room
                           transfer functions.
    band_hz              : centre frequencies of the EQ bands (default 10-band)
    max_gain_db          : maximum allowed gain magnitude per band
    use_harman_target    : blend toward Harman 2018 target instead of flat
    harman_blend         : 0 = pure flat target, 1 = pure Harman target
    auto_preamp          : if True, return a negative preamp to avoid clipping
    q                    : Q the playback chain will use for every band. Only
                           affects the auto-preamp headroom calculation (the
                           per-band gains themselves don't depend on it), but
                           it must match what actually gets applied or the
                           headroom is computed for the wrong filter shape.
    sample_rate          : sample rate the filters will run at; likewise only
                           used for the headroom calculation.

    Returns
    -------
    gains_db   : np.ndarray shape (N,) — one value per band
    preamp_db  : float — suggested global preamp (≤ 0 dB)
    """
    if band_hz is None:
        band_hz = DEFAULT_BAND_HZ
    bands = np.asarray(band_hz, dtype=float)

    # Interpolate measured response at band centres (log-frequency space)
    log_freqs  = np.log10(np.maximum(freqs, 1e-6))
    log_bands  = np.log10(bands)

    # np.interp clamps to the endpoint value outside the measured range rather
    # than extrapolating or erroring. That is silently wrong for bands the
    # measurement never covered -- e.g. the 16 kHz band against a 32 kHz-rate
    # recording gets whatever the response happened to be at Nyquist, and a
    # correction is then confidently generated from it. Warn instead of
    # letting it pass unnoticed.
    usable = freqs[freqs >= MIN_FREQ_HZ]
    f_min = float(usable.min()) if usable.size else MIN_FREQ_HZ
    f_max = float(freqs.max()) if np.size(freqs) else MIN_FREQ_HZ
    out_of_range = bands[(bands < f_min) | (bands > f_max)]
    if out_of_range.size:
        warnings.warn(
            "Band centre(s) "
            + ", ".join(f"{b:g} Hz" for b in out_of_range)
            + f" lie outside the measured range [{f_min:g}, {f_max:g}] Hz; "
              "their correction is extrapolated from the nearest measured bin "
              "and should not be trusted.",
            UserWarning,
            stacklevel=2,
        )

    meas_at_bands = np.interp(log_bands, log_freqs, magnitude_db)

    # Correction = invert the measured response (target = flat = 0 dBFS)
    gains_db = -meas_at_bands

    # Reference to 0 dB at 1 kHz. Measured levels from measurement.py are on an
    # arbitrary (uncalibrated) scale, so every band's correction is expressed
    # relative to whatever the response happens to measure at 1 kHz, by
    # convention. gain(f) = level(1kHz) - level(f).
    ref_1khz_db = np.interp(np.log10(1000.0), log_freqs, magnitude_db)
    gains_db += ref_1khz_db

    # Optionally blend toward Harman target
    if use_harman_target:
        harman_blend = float(np.clip(harman_blend, 0.0, 1.0))
        harman_at_bands = np.interp(bands, _HARMAN_HZ, _HARMAN_DB)
        gains_db = (1.0 - harman_blend) * gains_db + harman_blend * (-meas_at_bands + ref_1khz_db + harman_at_bands)

    # Clip per-band
    gains_db = np.clip(gains_db, -max_gain_db, max_gain_db)

    # Preamp: headroom so the filter chain's actual peak doesn't clip.
    #
    # This used to be -max(gains_db), i.e. the largest single band gain. That
    # under-estimates the headroom needed, because the bands are a *cascade*
    # of peaking biquads that overlap: at the default Q of 1.0 each band is
    # roughly an octave wide while the centres are an octave apart, so two
    # adjacent boosts sum and the combined response between them exceeds
    # either one alone. The daemon's output clamp then hard-clips exactly the
    # difference. cascade_peak_db() evaluates the real summed response.
    preamp_db = 0.0
    if auto_preamp:
        peak_db = cascade_peak_db(bands, gains_db, q=q, sample_rate=sample_rate)
        if peak_db > 0:
            preamp_db = -peak_db

    return gains_db, preamp_db


def apply_octave_normalisation(
    band_hz: Sequence[float],
    gains_db: np.ndarray,
) -> np.ndarray:
    """
    Normalise so the median band gain is 0 dB (centres the curve).
    Useful when you want perceptual levelling rather than absolute correction.
    """
    return gains_db - np.median(gains_db)
