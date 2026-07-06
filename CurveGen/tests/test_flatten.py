"""
tests/test_flatten.py — Unit tests for the flatten.py correction algorithm.
"""
import numpy as np
import pytest
from curvegen.flatten import compute_correction, apply_octave_normalisation, DEFAULT_BAND_HZ


def make_flat_response(n: int = 2048, sr: float = 48000.0):
    """Return a perfectly flat (0 dB) frequency response."""
    freqs = np.fft.rfftfreq(n, d=1.0 / sr)
    return freqs, np.zeros_like(freqs)


def make_boosted_response(boost_hz: float, boost_db: float, n: int = 2048, sr: float = 48000.0):
    """Return a response with a Gaussian peak at boost_hz."""
    freqs = np.fft.rfftfreq(n, d=1.0 / sr)
    mag   = boost_db * np.exp(-0.5 * ((np.log10(np.maximum(freqs, 1)) - np.log10(boost_hz)) / 0.3) ** 2)
    return freqs, mag


# ── Tests ─────────────────────────────────────────────────────────────────────

class TestComputeCorrection:

    def test_flat_input_gives_zero_correction(self):
        freqs, mag = make_flat_response()
        gains, preamp = compute_correction(freqs, mag)
        assert gains.shape == (10,)
        np.testing.assert_allclose(gains, 0.0, atol=0.5)

    def test_correction_inverts_boost(self):
        """A +6 dB peak at 4 kHz (relative to the 1 kHz reference) should
        produce a negative correction there.

        Note: a boost located exactly AT 1 kHz is a degenerate case for this
        algorithm, since every band's gain is computed relative to whatever
        the response measures at 1 kHz (see compute_correction's docstring/
        comments) — a peak at the reference frequency itself always
        corrects to ~0 by construction. So this test uses a different band
        to exercise the actual inversion behaviour.
        """
        freqs, mag = make_boosted_response(4000, 6.0)
        gains, _ = compute_correction(freqs, mag, auto_preamp=False)
        # Band index 7 is 4000 Hz
        assert gains[7] < -3.0, f"Expected negative correction at 4kHz, got {gains[7]:.2f}"

    def test_boost_at_reference_frequency_is_self_cancelling(self):
        """A boost located exactly at 1 kHz (the reference point) should
        correct to ~0, since gains are defined relative to the 1 kHz level."""
        freqs, mag = make_boosted_response(1000, 6.0)
        gains, _ = compute_correction(freqs, mag, auto_preamp=False)
        assert abs(gains[5]) < 0.5, f"Expected ~0 correction at 1kHz, got {gains[5]:.2f}"

    def test_gains_clipped_to_max(self):
        """Gains must never exceed max_gain_db."""
        freqs, mag = make_boosted_response(500, 30.0)
        gains, _ = compute_correction(freqs, mag, max_gain_db=12.0)
        assert np.all(np.abs(gains) <= 12.0 + 1e-6)

    def test_auto_preamp_is_non_positive(self):
        freqs, mag = make_boosted_response(125, -8.0)
        _, preamp = compute_correction(freqs, mag, auto_preamp=True)
        assert preamp <= 0.0

    def test_harman_enabled(self):
        freqs, mag = make_flat_response()
        gains_flat, _   = compute_correction(freqs, mag, use_harman_target=False)
        gains_harman, _ = compute_correction(freqs, mag, use_harman_target=True)
        # With Harman target on a flat input the low frequencies should be boosted.
        assert gains_harman[0] > gains_flat[0]

    def test_output_length_matches_bands(self):
        freqs, mag = make_flat_response()
        for n_bands in [5, 10, 15]:
            band_hz = np.logspace(np.log10(20), np.log10(20000), n_bands)
            gains, _ = compute_correction(freqs, mag, band_hz=band_hz)
            assert len(gains) == n_bands


class TestOctaveNormalisation:

    def test_median_is_zero_after_normalisation(self):
        gains = np.array([3.0, -2.0, 1.0, 4.0, -1.0, 0.0, 2.0, -3.0, 1.0, 0.5])
        normalised = apply_octave_normalisation(DEFAULT_BAND_HZ, gains)
        assert abs(np.median(normalised)) < 1e-9
