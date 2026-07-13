"""
tests/test_visualize.py — Unit tests for visualize.py: the RBJ biquad
response evaluator (must match DSP::Biquad::SetPeaking's math, see
DSP/Biquad.cpp) and the 4-stage report builder.
"""
from __future__ import annotations

import matplotlib
matplotlib.use("Agg")  # headless-safe backend; must be set before pyplot is imported

import numpy as np
import pytest
from scipy.io import wavfile

from curvegen import visualize


def _write_wav(path, sr, data):
    wavfile.write(str(path), sr, data)


def _sine(freq, sr, duration=1.0, amplitude=0.5):
    n = int(sr * duration)
    t = np.arange(n) / sr
    return (amplitude * np.sin(2.0 * np.pi * freq * t)).astype(np.float32)


# ── evaluate_eq_response_db / biquad math ────────────────────────────────────

class TestEvaluateEqResponseDb:

    def test_zero_gain_is_unity_everywhere(self):
        # Same property ARCHITECTURE.md documents for DSP::Biquad at 0 dB:
        # b1==a1 and b2==a2 exactly, so H(z) == 1 for every frequency.
        freqs = np.array([31, 100, 500, 1000, 4000, 16000], dtype=float)
        response = visualize.evaluate_eq_response_db(freqs, [1000.0], [0.0], q=1.0, sample_rate=48000.0)
        np.testing.assert_allclose(response, 0.0, atol=1e-6)

    def test_response_at_center_frequency_equals_gain(self):
        # A peaking filter's response *at its own design frequency* equals
        # its configured gain exactly, independent of Q.
        for gain in (-9.0, -3.0, 3.0, 9.0):
            for q in (0.5, 1.0, 4.0):
                response = visualize.evaluate_eq_response_db(
                    [1000.0], [1000.0], [gain], q=q, sample_rate=48000.0,
                )
                assert response[0] == pytest.approx(gain, abs=1e-4)

    def test_far_from_center_frequency_response_decays_toward_zero(self):
        response = visualize.evaluate_eq_response_db(
            [20.0], [1000.0], [12.0], q=1.0, sample_rate=48000.0,
        )
        assert abs(response[0]) < 1.0  # far below the boosted band -> near unity

    def test_cascaded_bands_are_additive_in_db(self):
        freqs = np.logspace(np.log10(20), np.log10(20000), 50)
        combined = visualize.evaluate_eq_response_db(
            freqs, [1000.0, 2000.0], [3.0, -4.0], q=1.0, sample_rate=48000.0,
        )
        band1 = visualize.evaluate_eq_response_db(freqs, [1000.0], [3.0], q=1.0, sample_rate=48000.0)
        band2 = visualize.evaluate_eq_response_db(freqs, [2000.0], [-4.0], q=1.0, sample_rate=48000.0)
        np.testing.assert_allclose(combined, band1 + band2, atol=1e-9)

    def test_mismatched_band_gain_lengths_raises(self):
        with pytest.raises(ValueError, match="length"):
            visualize.evaluate_eq_response_db([1000.0], [1000.0, 2000.0], [3.0], sample_rate=48000.0)

    def test_mismatched_q_length_raises(self):
        with pytest.raises(ValueError, match="q length"):
            visualize.evaluate_eq_response_db(
                [1000.0], [1000.0, 2000.0], [3.0, 1.0], q=[1.0], sample_rate=48000.0,
            )

    def test_scalar_and_per_band_q_agree_when_equal(self):
        freqs = np.logspace(np.log10(20), np.log10(20000), 30)
        a = visualize.evaluate_eq_response_db(freqs, [1000.0, 2000.0], [3.0, 1.0], q=1.5, sample_rate=48000.0)
        b = visualize.evaluate_eq_response_db(freqs, [1000.0, 2000.0], [3.0, 1.0], q=[1.5, 1.5], sample_rate=48000.0)
        np.testing.assert_allclose(a, b)


class TestSyntheticFreqGrid:

    def test_bounds_and_monotonic(self):
        grid = visualize.synthetic_freq_grid(48000.0)
        assert grid[0] == pytest.approx(visualize.FFT_GRID_MIN_HZ, rel=1e-6)
        assert grid[-1] <= 48000.0 / 2.0
        assert np.all(np.diff(grid) > 0)
        assert len(grid) == visualize.FFT_GRID_POINTS

    def test_low_sample_rate_clamps_upper_bound(self):
        grid = visualize.synthetic_freq_grid(8000.0)
        assert grid[-1] < 4000.0


# ── build_report ──────────────────────────────────────────────────────────────

class TestBuildReport:

    def test_stage4_unavailable_without_recorded_output(self, tmp_path):
        wav_path = tmp_path / "input.wav"
        _write_wav(wav_path, 48000, _sine(500.0, 48000, duration=2.0))

        report = visualize.build_report(str(wav_path))

        assert report.recorded_input.available
        assert report.curve_generated.available
        assert report.expected_output.available
        assert not report.recorded_output.available
        assert report.recorded_output.note  # explains why

    def test_stage4_available_with_recorded_output(self, tmp_path):
        in_path = tmp_path / "input.wav"
        out_path = tmp_path / "output.wav"
        _write_wav(in_path, 48000, _sine(500.0, 48000, duration=2.0))
        _write_wav(out_path, 48000, _sine(500.0, 48000, duration=2.0, amplitude=0.3))

        report = visualize.build_report(str(in_path), recorded_output_path=str(out_path))

        assert report.recorded_output.available
        assert report.recorded_output.freqs is not None
        assert len(report.recorded_output.freqs) == len(report.recorded_output.fft_db)

    def test_all_stages_have_matching_fft_cpb_shapes(self, tmp_path):
        wav_path = tmp_path / "input.wav"
        _write_wav(wav_path, 48000, _sine(500.0, 48000, duration=2.0))
        report = visualize.build_report(str(wav_path))

        for stage in [report.recorded_input, report.curve_generated, report.expected_output]:
            assert stage.freqs.shape == stage.fft_db.shape
            assert stage.cpb_freqs.shape == stage.cpb_db.shape

    def test_expected_output_equals_input_plus_curve(self, tmp_path):
        """
        Regression check on the actual arithmetic: expected_output's raw
        (FFT-resolution) curve must equal recorded_input (interpolated onto
        the same grid) + curve_generated + preamp, at every point on the
        shared grid -- not just approximately/visually.
        """
        wav_path = tmp_path / "input.wav"
        _write_wav(wav_path, 48000, _sine(500.0, 48000, duration=2.0))
        report = visualize.build_report(str(wav_path))

        grid = report.curve_generated.freqs  # same grid used for stage 2 and 3
        np.testing.assert_allclose(report.expected_output.freqs, grid)

        log_freqs_in = np.log10(np.maximum(report.recorded_input.freqs, 1e-6))
        interp_input = np.interp(np.log10(grid), log_freqs_in, report.recorded_input.fft_db)
        expected = interp_input + report.curve_generated.fft_db  # curve_generated already includes preamp
        np.testing.assert_allclose(report.expected_output.fft_db, expected, atol=1e-9)

    def test_band_hz_and_gains_have_matching_length(self, tmp_path):
        wav_path = tmp_path / "input.wav"
        _write_wav(wav_path, 48000, _sine(500.0, 48000, duration=2.0))
        report = visualize.build_report(str(wav_path))
        assert len(report.band_hz) == len(report.gains_db)


# ── plot_report (rendering smoke test) ───────────────────────────────────────

class TestPlotReport:

    def test_renders_png_with_all_four_stages(self, tmp_path):
        in_path = tmp_path / "input.wav"
        out_path = tmp_path / "output.wav"
        report_path = tmp_path / "report.png"
        _write_wav(in_path, 48000, _sine(500.0, 48000, duration=2.0))
        _write_wav(out_path, 48000, _sine(500.0, 48000, duration=2.0, amplitude=0.3))

        report = visualize.build_report(str(in_path), recorded_output_path=str(out_path))
        visualize.plot_report(report, str(report_path))

        assert report_path.exists()
        assert report_path.stat().st_size > 0

    def test_renders_png_with_stage4_unavailable(self, tmp_path):
        in_path = tmp_path / "input.wav"
        report_path = tmp_path / "report.png"
        _write_wav(in_path, 48000, _sine(500.0, 48000, duration=2.0))

        report = visualize.build_report(str(in_path))
        visualize.plot_report(report, str(report_path))

        assert report_path.exists()
        assert report_path.stat().st_size > 0
