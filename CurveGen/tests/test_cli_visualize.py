"""
tests/test_cli_visualize.py — CLI integration test for `eq-curvegen visualize`.
"""
from __future__ import annotations

import matplotlib
matplotlib.use("Agg")  # headless-safe backend; must be set before pyplot is imported

import numpy as np
import pytest
from scipy.io import wavfile

from curvegen import cli


def _write_wav(path, sr, data):
    wavfile.write(str(path), sr, data)


def _sine(freq, sr, duration=1.0, amplitude=0.5):
    n = int(sr * duration)
    t = np.arange(n) / sr
    return (amplitude * np.sin(2.0 * np.pi * freq * t)).astype(np.float32)


def _run_cli(monkeypatch, argv):
    monkeypatch.setattr("sys.argv", ["eq-curvegen"] + argv)
    with pytest.raises(SystemExit) as exc_info:
        cli.main()
    return exc_info.value.code


class TestVisualizeCommand:

    def test_produces_report_without_recorded_output(self, tmp_path, monkeypatch, capsys):
        in_path = tmp_path / "input.wav"
        report_path = tmp_path / "report.png"
        _write_wav(in_path, 48000, _sine(500.0, 48000, duration=2.0))

        code = _run_cli(monkeypatch, [
            "visualize",
            "--input", str(in_path),
            "--output", str(report_path),
        ])

        assert code == 0
        assert report_path.exists()
        assert report_path.stat().st_size > 0
        out = capsys.readouterr().out
        assert "none supplied" in out

    def test_produces_report_with_recorded_output(self, tmp_path, monkeypatch, capsys):
        in_path = tmp_path / "input.wav"
        out_wav_path = tmp_path / "output.wav"
        report_path = tmp_path / "report.png"
        _write_wav(in_path, 48000, _sine(500.0, 48000, duration=2.0))
        _write_wav(out_wav_path, 48000, _sine(500.0, 48000, duration=2.0, amplitude=0.3))

        code = _run_cli(monkeypatch, [
            "visualize",
            "--input", str(in_path),
            "--recorded-output", str(out_wav_path),
            "--output", str(report_path),
        ])

        assert code == 0
        assert report_path.exists()
        assert report_path.stat().st_size > 0
        out = capsys.readouterr().out
        assert "Recorded output:" in out
        assert str(out_wav_path) in out

    def test_prints_requested_band_gains(self, tmp_path, monkeypatch, capsys):
        in_path = tmp_path / "input.wav"
        report_path = tmp_path / "report.png"
        _write_wav(in_path, 48000, _sine(500.0, 48000, duration=2.0))

        code = _run_cli(monkeypatch, [
            "visualize",
            "--input", str(in_path),
            "--output", str(report_path),
        ])

        assert code == 0
        out = capsys.readouterr().out
        assert "Requested gain (dB)" in out
        assert "Preamp:" in out

    def test_explicit_input_format_is_honoured(self, tmp_path, monkeypatch):
        in_path = tmp_path / "input.wav"
        report_path = tmp_path / "report.png"
        _write_wav(in_path, 48000, _sine(500.0, 48000, duration=2.0))

        code = _run_cli(monkeypatch, [
            "visualize",
            "--input", str(in_path),
            "--input-format", "wav",
            "--output", str(report_path),
        ])

        assert code == 0
        assert report_path.exists()

    def test_missing_required_input_argument_errors(self, monkeypatch):
        monkeypatch.setattr("sys.argv", ["eq-curvegen", "visualize", "--output", "report.png"])
        with pytest.raises(SystemExit) as exc_info:
            cli.main()
        assert exc_info.value.code != 0
