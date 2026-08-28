"""
response.py — Continuous magnitude response of the peaking-biquad EQ cascade.

Extracted from visualize.py so that flatten.py can use it too without
importing visualize (which imports flatten -- a cycle). flatten.py needs it to
compute how much headroom the *cascade* actually consumes, which is not the
same as the largest single band gain: with the default Q of 1.0 the ten bands
overlap substantially, so adjacent boosts sum.

This must stay numerically in step with DSP/Biquad.cpp's SetPeaking(): both
implement the same RBJ ("Audio EQ Cookbook") peaking-EQ formula, verified side
by side, not derived independently. If Biquad.cpp's formula ever changes, this
needs to change with it or "curve generated" / "expected output" /
auto-preamp stop reflecting what the real DSP does.
"""
from __future__ import annotations

import numpy as np
from typing import Sequence, Union

DEFAULT_SAMPLE_RATE = 48000.0


def _peaking_biquad_coeffs(center_hz: float, q: float, gain_db: float, sample_rate: float):
    sr = sample_rate if sample_rate and sample_rate > 0 else DEFAULT_SAMPLE_RATE
    omega = 2.0 * np.pi * center_hz / sr
    sinw = np.sin(omega)
    cosw = np.cos(omega)
    A = 10.0 ** (gain_db / 40.0)  # sqrt of linear power gain
    alpha = sinw / (2.0 * q)

    b0 = 1.0 + alpha * A
    b1 = -2.0 * cosw
    b2 = 1.0 - alpha * A
    a0 = 1.0 + alpha / A
    a1 = -2.0 * cosw
    a2 = 1.0 - alpha / A

    return b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0


def _biquad_magnitude_db(freqs_hz: np.ndarray, b0, b1, b2, a1, a2, sample_rate: float) -> np.ndarray:
    """|H(e^jw)| in dB for a normalised (a0=1) biquad, at each frequency in freqs_hz."""
    sr = sample_rate if sample_rate and sample_rate > 0 else DEFAULT_SAMPLE_RATE
    w = 2.0 * np.pi * np.asarray(freqs_hz, dtype=float) / sr
    z_inv = np.exp(-1j * w)
    num = b0 + b1 * z_inv + b2 * z_inv ** 2
    den = 1.0 + a1 * z_inv + a2 * z_inv ** 2
    h = num / den
    return 20.0 * np.log10(np.maximum(np.abs(h), 1e-12))


def evaluate_eq_response_db(
    freqs_hz: Sequence[float],
    band_hz: Sequence[float],
    gains_db: Sequence[float],
    q: Union[float, Sequence[float]] = 1.0,
    sample_rate: float = DEFAULT_SAMPLE_RATE,
) -> np.ndarray:
    """
    Continuous magnitude response (dB) of the cascaded N-band peaking EQ
    DSP::Equalizer10Band would apply, evaluated at `freqs_hz`. Cascaded
    (series) biquads multiply in linear magnitude, i.e. add in dB, so this
    is a plain sum over bands -- matching Equalizer10Band::Process()'s
    back-to-back band chain (§2.2 in ARCHITECTURE.md).

    Does NOT include preamp; callers add that separately (a constant dB
    offset), since preamp is a separate, simpler stage of the chain.
    """
    freqs_hz = np.asarray(freqs_hz, dtype=float)
    if len(band_hz) != len(gains_db):
        raise ValueError(
            f"band_hz length ({len(band_hz)}) must match gains_db length ({len(gains_db)})"
        )
    if isinstance(q, (int, float)):
        q_values = [float(q)] * len(band_hz)
    else:
        q_values = [float(v) for v in q]
        if len(q_values) != len(band_hz):
            raise ValueError(f"q length ({len(q_values)}) must match band_hz length ({len(band_hz)})")

    total_db = np.zeros_like(freqs_hz)
    for hz, gain, qv in zip(band_hz, gains_db, q_values):
        b0, b1, b2, a1, a2 = _peaking_biquad_coeffs(float(hz), qv, float(gain), sample_rate)
        total_db += _biquad_magnitude_db(freqs_hz, b0, b1, b2, a1, a2, sample_rate)
    return total_db


def cascade_peak_db(
    band_hz: Sequence[float],
    gains_db: Sequence[float],
    q: Union[float, Sequence[float]] = 1.0,
    sample_rate: float = DEFAULT_SAMPLE_RATE,
    points: int = 1024,
) -> float:
    """
    Largest positive value of the cascaded EQ's magnitude response, in dB --
    i.e. the real worst-case boost the filter chain applies to any frequency.

    This is what auto-preamp needs. `max(gains_db)` is an under-estimate: two
    neighbouring +6 dB bands at Q=1 overlap enough that the response between
    them exceeds +6 dB, and the daemon's output clamp then hard-clips the
    difference. Returns 0.0 if the cascade never exceeds unity.
    """
    if len(band_hz) == 0:
        return 0.0
    sr = sample_rate if sample_rate and sample_rate > 0 else DEFAULT_SAMPLE_RATE
    fmax = min(20000.0, sr / 2.0 * 0.99)
    fmin = min(10.0, fmax / 2.0)
    grid = np.logspace(np.log10(fmin), np.log10(fmax), points)
    response = evaluate_eq_response_db(grid, band_hz, gains_db, q=q, sample_rate=sr)
    return float(max(0.0, np.max(response)))
