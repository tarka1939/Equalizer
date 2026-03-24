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

import numpy as np
from typing import Optional, Sequence, Tuple

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
) -> Tuple[np.ndarray, float]:
    """
    Compute per-band correction gains that flatten the measured response.

    Parameters
    ----------
    freqs, magnitude_db  : measured frequency response (from measurement.py)
    band_hz              : centre frequencies of the EQ bands (default 10-band)
    max_gain_db          : maximum allowed gain magnitude per band
    use_harman_target    : blend toward Harman 2018 target instead of flat
    harman_blend         : 0 = pure flat target, 1 = pure Harman target
    auto_preamp          : if True, return a negative preamp to avoid clipping

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
    meas_at_bands = np.interp(log_bands, log_freqs, magnitude_db)

    # Correction = invert the measured response (target = flat = 0 dBFS)
    gains_db = -meas_at_bands

    # Reference to 0 dB at 1 kHz
    gains_db -= np.interp(np.log10(1000.0), log_freqs, magnitude_db)

    # Optionally blend toward Harman target
    if use_harman_target:
        harman_at_bands = np.interp(bands, _HARMAN_HZ, _HARMAN_DB)
        gains_db = (1.0 - harman_blend) * gains_db + harman_blend * (-meas_at_bands + harman_at_bands)

    # Clip per-band
    gains_db = np.clip(gains_db, -max_gain_db, max_gain_db)

    # Preamp: headroom so the highest boost doesn't clip
    preamp_db = 0.0
    if auto_preamp:
        max_boost = float(np.max(gains_db))
        if max_boost > 0:
            preamp_db = -max_boost   # e.g. if +6 dB band, preamp = -6 dB

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
