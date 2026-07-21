"""
tests/test_loaders.py — Unit tests for loaders.py's pluggable measurement
loader registry.
"""
from __future__ import annotations

import numpy as np
import pytest
from scipy.io import wavfile

from curvegen import loaders


def _write_wav(path, sr, data):
    wavfile.write(str(path), sr, data)


def _sine(freq, sr, duration=1.0, amplitude=0.5):
    n = int(sr * duration)
    t = np.arange(n) / sr
    return (amplitude * np.sin(2.0 * np.pi * freq * t)).astype(np.float32)


class TestBuiltinWavLoader:

    def test_wav_registered_and_available(self):
        assert "wav" in loaders.available_formats()

    def test_detect_format_from_extension(self, tmp_path):
        path = tmp_path / "measurement.wav"
        assert loaders.detect_format(str(path)) == "wav"

    def test_detect_format_uppercase_extension(self, tmp_path):
        path = tmp_path / "measurement.WAV"
        assert loaders.detect_format(str(path)) == "wav"

    def test_detect_format_unknown_extension_returns_none(self, tmp_path):
        path = tmp_path / "measurement.mdat"
        assert loaders.detect_format(str(path)) is None

    def test_load_auto_detects_wav(self, tmp_path):
        path = tmp_path / "room.wav"
        _write_wav(path, 48000, _sine(1000.0, 48000, duration=1.0))
        freqs, mag_db, sr = loaders.load(str(path))
        assert len(freqs) == len(mag_db)
        assert sr == pytest.approx(48000.0)

    def test_load_falls_back_to_wav_for_unrecognised_extension(self, tmp_path):
        # No extension at all -> detect_format returns None -> falls back to "wav".
        path = tmp_path / "room_no_ext"
        _write_wav(path, 48000, _sine(1000.0, 48000, duration=1.0))
        freqs, mag_db, sr = loaders.load(str(path))
        assert len(freqs) == len(mag_db)

    def test_load_explicit_format_overrides_detection(self, tmp_path):
        path = tmp_path / "room.wav"
        _write_wav(path, 48000, _sine(1000.0, 48000, duration=1.0))
        freqs, mag_db, sr = loaders.load(str(path), fmt="wav")
        assert len(freqs) == len(mag_db)

    def test_load_ir_flag_uses_impulse_response_path(self, tmp_path):
        path = tmp_path / "ir.wav"
        _write_wav(path, 48000, _sine(1000.0, 48000, duration=0.1))
        freqs, mag_db, sr = loaders.load(str(path), ir=True)
        assert len(freqs) == len(mag_db)

    def test_unknown_format_raises_with_helpful_message(self, tmp_path):
        path = tmp_path / "room.wav"
        _write_wav(path, 48000, _sine(1000.0, 48000, duration=0.1))
        with pytest.raises(ValueError, match="Unknown measurement format"):
            loaders.load(str(path), fmt="nonexistent_format")


class TestRegisterLoader:

    def test_register_new_loader_becomes_available(self):
        def _dummy_loader(path, channel=0, ir=False):
            return np.array([100.0, 1000.0]), np.array([0.0, 0.0]), 48000.0

        loaders.register_loader("dummy_test_format", _dummy_loader, extensions=[".dummytest"])
        try:
            assert "dummy_test_format" in loaders.available_formats()
            freqs, mag_db, sr = loaders.load("whatever.dummytest")
            np.testing.assert_array_equal(freqs, [100.0, 1000.0])
        finally:
            # Clean up so this test doesn't leak state into others.
            del loaders._LOADERS["dummy_test_format"]
            del loaders._EXTENSIONS[".dummytest"]

    def test_reregistering_a_name_overwrites_it(self):
        def _v1(path, channel=0, ir=False):
            return np.array([1.0]), np.array([1.0]), 1.0

        def _v2(path, channel=0, ir=False):
            return np.array([2.0]), np.array([2.0]), 2.0

        loaders.register_loader("overwrite_test", _v1)
        loaders.register_loader("overwrite_test", _v2)
        try:
            freqs, mag_db, sr = loaders.load("x", fmt="overwrite_test")
            assert freqs[0] == 2.0
        finally:
            del loaders._LOADERS["overwrite_test"]
