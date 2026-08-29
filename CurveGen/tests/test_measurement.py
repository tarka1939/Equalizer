"""
tests/test_measurement.py — Unit tests for measurement.py (WAV loading, PSD/FFT
analysis, PCM normalisation, fractional-octave smoothing).
"""
from __future__ import annotations

import numpy as np
import pytest
from scipy.io import wavfile

from curvegen import measurement


# ── Fixtures / helpers ──────────────────────────────────────────────────────────

def _write_wav(path, sr, data):
    wavfile.write(str(path), sr, data)


def _sine(freq, sr, duration=1.0, amplitude=0.5):
    n = int(sr * duration)
    t = np.arange(n) / sr
    return (amplitude * np.sin(2.0 * np.pi * freq * t)).astype(np.float32)


# ── _norm_audio ──────────────────────────────────────────────────────────────────

class TestNormAudio:

    def test_int16_normalised_to_unit_range(self):
        data = np.array([-32768, 0, 32767], dtype=np.int16)
        out = measurement._norm_audio(data)
        assert out.dtype == np.float32
        np.testing.assert_allclose(out, [-1.0, 0.0, 32767 / 32768.0], atol=1e-6)

    def test_int32_normalised_to_unit_range(self):
        data = np.array([-2_147_483_648, 0, 2_147_483_647], dtype=np.int32)
        out = measurement._norm_audio(data)
        assert out.dtype == np.float32
        np.testing.assert_allclose(out, [-1.0, 0.0, 1.0], atol=1e-6)

    def test_uint8_centred_and_normalised(self):
        data = np.array([0, 128, 255], dtype=np.uint8)
        out = measurement._norm_audio(data)
        assert out.dtype == np.float32
        np.testing.assert_allclose(out, [-1.0, 0.0, 127 / 128.0], atol=1e-6)

    def test_float32_passthrough(self):
        data = np.array([-1.0, 0.25, 1.0], dtype=np.float32)
        out = measurement._norm_audio(data)
        assert out.dtype == np.float32
        np.testing.assert_allclose(out, data)

    def test_float64_cast_to_float32(self):
        data = np.array([-0.5, 0.0, 0.5], dtype=np.float64)
        out = measurement._norm_audio(data)
        assert out.dtype == np.float32
        np.testing.assert_allclose(out, data, atol=1e-6)


# ── load_wav ──────────────────────────────────────────────────────────────────

class TestLoadWav:

    def test_returns_expected_shapes_and_types(self, tmp_path):
        sr = 48000
        data = _sine(1000, sr)
        path = tmp_path / "sine.wav"
        _write_wav(path, sr, data)

        freqs, mag_db, sr_out = measurement.load_wav(str(path))
        assert sr_out == sr
        assert freqs.shape == mag_db.shape
        assert freqs[0] == 0.0
        assert np.all(np.diff(freqs) > 0)  # monotonically increasing

    def test_peak_is_at_the_correct_frequency(self, tmp_path):
        sr = 48000
        freq = 2000.0
        data = _sine(freq, sr, duration=2.0, amplitude=0.8)
        path = tmp_path / "tone.wav"
        _write_wav(path, sr, data)

        freqs, mag_db, _ = measurement.load_wav(str(path))
        peak_freq = freqs[np.argmax(mag_db)]
        # Welch's frequency resolution with nperseg=4096 @ 48kHz is ~11.7 Hz/bin.
        assert abs(peak_freq - freq) < 20.0

    def test_mono_file_ignores_channel_arg(self, tmp_path):
        sr = 44100
        data = _sine(500, sr)
        path = tmp_path / "mono.wav"
        _write_wav(path, sr, data)

        freqs0, mag0, _ = measurement.load_wav(str(path), channel=0)
        freqs1, mag1, _ = measurement.load_wav(str(path), channel=1)
        np.testing.assert_allclose(mag0, mag1)

    def test_stereo_channel_selection(self, tmp_path):
        sr = 44100
        left  = _sine(500, sr)
        right = _sine(5000, sr)
        stereo = np.stack([left, right], axis=1)
        path = tmp_path / "stereo.wav"
        _write_wav(path, sr, stereo)

        freqs_l, mag_l, _ = measurement.load_wav(str(path), channel=0)
        freqs_r, mag_r, _ = measurement.load_wav(str(path), channel=1)

        peak_l = freqs_l[np.argmax(mag_l)]
        peak_r = freqs_r[np.argmax(mag_r)]
        assert abs(peak_l - 500) < 20.0
        assert abs(peak_r - 5000) < 20.0

    def test_stereo_channel_index_wraps_modulo_channel_count(self, tmp_path):
        """channel=2 on a 2-channel file should wrap to channel 0 (channel % num_channels)."""
        sr = 44100
        left  = _sine(500, sr)
        right = _sine(5000, sr)
        stereo = np.stack([left, right], axis=1)
        path = tmp_path / "stereo2.wav"
        _write_wav(path, sr, stereo)

        freqs0, mag0, _ = measurement.load_wav(str(path), channel=0)
        freqs2, mag2, _ = measurement.load_wav(str(path), channel=2)
        np.testing.assert_allclose(mag0, mag2)

    def test_int16_file_loads_without_error(self, tmp_path):
        sr = 44100
        data = (_sine(1000, sr) * 32767).astype(np.int16)
        path = tmp_path / "pcm16.wav"
        _write_wav(path, sr, data)

        freqs, mag_db, sr_out = measurement.load_wav(str(path))
        assert sr_out == sr
        assert np.isfinite(mag_db).all()


# ── load_impulse_response ─────────────────────────────────────────────────────

class TestLoadImpulseResponse:

    def test_returns_expected_shapes(self, tmp_path):
        sr = 48000
        data = _sine(1000, sr, duration=0.5)
        path = tmp_path / "ir.wav"
        _write_wav(path, sr, data)

        freqs, mag_db, sr_out = measurement.load_impulse_response(str(path))
        assert sr_out == sr
        assert freqs.shape == mag_db.shape
        assert np.isfinite(mag_db).all()

    def test_peak_is_at_the_correct_frequency(self, tmp_path):
        sr = 48000
        freq = 4000.0
        data = _sine(freq, sr, duration=1.0, amplitude=0.9)
        path = tmp_path / "ir_tone.wav"
        _write_wav(path, sr, data)

        freqs, mag_db, _ = measurement.load_impulse_response(str(path))
        peak_freq = freqs[np.argmax(mag_db)]
        assert abs(peak_freq - freq) < 5.0  # FFT-based: fine resolution

    def test_stereo_channel_selection(self, tmp_path):
        sr = 48000
        left  = _sine(300, sr)
        right = _sine(6000, sr)
        stereo = np.stack([left, right], axis=1)
        path = tmp_path / "ir_stereo.wav"
        _write_wav(path, sr, stereo)

        freqs_l, mag_l, _ = measurement.load_impulse_response(str(path), channel=0)
        freqs_r, mag_r, _ = measurement.load_impulse_response(str(path), channel=1)
        assert abs(freqs_l[np.argmax(mag_l)] - 300) < 5.0
        assert abs(freqs_r[np.argmax(mag_r)] - 6000) < 5.0

    @pytest.mark.parametrize("peak_offset", [0, 48, 480, 4800, 8192])
    def test_recovers_known_response_wherever_the_peak_sits(self, tmp_path, peak_offset):
        """
        Feed a real impulse response (not a sine) whose magnitude response is
        known in closed form, and check the recovered curve matches it.

        The other tests in this class all pass a *sine wave* to an
        impulse-response loader. A long sine has energy spread over the whole
        file, so the full-length Hann window applied here barely perturbs it
        and `argmax(mag_db)` still lands on the right frequency -- which is
        why they stayed green while the loader was mangling actual IRs. The
        window is zero at index 0 and one at the centre, so an IR whose peak
        sits near the start of the file (the normal case for an export) had
        its direct sound windowed away. Parametrised over peak position
        because the error was a smooth function of it: ~13.7 dB at offset 0,
        fading to nothing only once the peak reached mid-file, so a single
        fixed offset can pass while the loader is badly wrong elsewhere.
        """
        sr = 48000
        n = 16384
        fc, q, gain_db = 125.0, 1.0, 8.0

        # RBJ peaking biquad -- same shape as DSP::Biquad / Equalizer APO "PK".
        A = 10.0 ** (gain_db / 40.0)
        w0 = 2.0 * np.pi * fc / sr
        alpha = np.sin(w0) / (2.0 * q)
        cos_w0 = np.cos(w0)
        b = np.array([1 + alpha * A, -2 * cos_w0, 1 - alpha * A])
        a = np.array([1 + alpha / A, -2 * cos_w0, 1 - alpha / A])
        b, a = b / a[0], a / a[0]

        from scipy.signal import freqz, lfilter

        impulse = np.zeros(n, dtype=np.float64)
        impulse[0] = 1.0
        ir = lfilter(b, a, impulse)
        ir = np.concatenate([np.zeros(peak_offset), ir])[:n]

        path = tmp_path / f"ir_{peak_offset}.wav"
        _write_wav(path, sr, ir.astype(np.float32))

        freqs, mag_db, _ = measurement.load_impulse_response(str(path))

        # Compare to the analytic response, both referenced to 1 kHz so the
        # arbitrary absolute scale of either side cancels out.
        probe = np.array([fc, 1000.0])
        _, h = freqz(b, a, worN=2.0 * np.pi * probe / sr)
        expected = 20.0 * np.log10(np.abs(h))
        expected = expected[0] - expected[1]

        measured = np.interp(probe, freqs, mag_db)
        measured = measured[0] - measured[1]

        assert measured == pytest.approx(expected, abs=0.5)


# ── smooth_octave ─────────────────────────────────────────────────────────────

class TestSmoothOctave:

    def test_output_shape_matches_input(self):
        freqs = np.linspace(0, 24000, 2049)
        mag = np.random.default_rng(0).normal(size=freqs.shape)
        freqs_out, smoothed = measurement.smooth_octave(freqs, mag)
        assert freqs_out is freqs
        assert smoothed.shape == mag.shape

    def test_dc_bin_passthrough_unchanged(self):
        """freqs[0] == 0 must not divide-by-zero; it should pass through untouched."""
        freqs = np.array([0.0, 100.0, 200.0, 400.0, 800.0])
        mag = np.array([5.0, 1.0, 1.0, 1.0, 1.0])
        _, smoothed = measurement.smooth_octave(freqs, mag)
        assert smoothed[0] == 5.0

    def test_smooths_a_narrow_spike(self):
        """A single-bin spike surrounded by a flat floor should be pulled toward
        the floor once smoothed over a wide enough fractional-octave window."""
        freqs = np.linspace(100, 10000, 500)
        mag = np.zeros_like(freqs)
        spike_idx = 250
        mag[spike_idx] = 40.0
        _, smoothed = measurement.smooth_octave(freqs, mag, fraction=1.0)
        assert smoothed[spike_idx] < mag[spike_idx]

    def test_smaller_fraction_smooths_less(self):
        freqs = np.linspace(100, 10000, 500)
        mag = np.zeros_like(freqs)
        mag[250] = 40.0
        _, smoothed_narrow = measurement.smooth_octave(freqs, mag, fraction=1 / 12)
        _, smoothed_wide   = measurement.smooth_octave(freqs, mag, fraction=1.0)
        assert smoothed_narrow[250] >= smoothed_wide[250]

    def test_flat_input_stays_flat(self):
        freqs = np.linspace(20, 20000, 1000)
        mag = np.full_like(freqs, 3.0)
        _, smoothed = measurement.smooth_octave(freqs, mag)
        np.testing.assert_allclose(smoothed, 3.0)
