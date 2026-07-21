"""
tests/test_cli_eqapo.py — Integration test for the `eq-curvegen eqapo`
subcommand: exercises the real argparse wiring and the full
measurement -> flatten -> eqapo_export pipeline end to end, against a
synthetic WAV, rather than only unit-testing eqapo_export.py in isolation.

There is no existing CLI test coverage for `measure`/`plot`/`send` either
(see ARCHITECTURE.md's testing-strategy section); this is added specifically
because `eqapo` is the new offline-validation entry point this branch adds,
and its argparse wiring (subparser args, --q default, etc.) is exactly the
kind of thing that's easy to get subtly wrong without a test exercising it.
"""
from __future__ import annotations

import re
import sys

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


def _run_cli(argv, monkeypatch):
    monkeypatch.setattr(sys, "argv", ["eq-curvegen"] + argv)
    try:
        cli.main()
    except SystemExit as e:
        return e.code if e.code is not None else 0
    return 0


def test_eqapo_subcommand_writes_valid_config(tmp_path, monkeypatch):
    wav_path = tmp_path / "room.wav"
    out_path = tmp_path / "config.txt"
    _write_wav(wav_path, 48000, _sine(1000.0, 48000, duration=2.0))

    code = _run_cli(
        ["eqapo", "--input", str(wav_path), "--output", str(out_path)],
        monkeypatch,
    )

    assert code == 0
    assert out_path.exists()

    text = out_path.read_text(encoding="utf-8")
    assert re.search(r"^Preamp:\s*[-\d.]+\s*dB\s*$", text, re.MULTILINE)
    filter_lines = [l for l in text.splitlines() if l.startswith("Filter")]
    assert len(filter_lines) == 10
    for line in filter_lines:
        assert "ON PK" in line
        assert "Fc" in line and "Hz" in line
        assert "Gain" in line and "dB" in line
        assert "Q" in line


def test_eqapo_subcommand_respects_custom_q(tmp_path, monkeypatch):
    wav_path = tmp_path / "room.wav"
    out_path = tmp_path / "config.txt"
    _write_wav(wav_path, 48000, _sine(1000.0, 48000, duration=2.0))

    _run_cli(
        ["eqapo", "--input", str(wav_path), "--output", str(out_path), "--q", "4.2"],
        monkeypatch,
    )

    text = out_path.read_text(encoding="utf-8")
    q_values = re.findall(r"Q\s+([-\d.]+)\s*$", text, re.MULTILINE)
    assert len(q_values) == 10
    for q in q_values:
        assert abs(float(q) - 4.2) < 0.01


def test_eqapo_and_measure_agree_on_underlying_curve(tmp_path, monkeypatch):
    """
    `eqapo` must derive its curve from the exact same analysis pipeline as
    `measure` (see cli.py's shared `_analyse()` helper) -- this pins that
    down by running both against the same input and comparing gains/preamp.
    """
    import json

    wav_path = tmp_path / "room.wav"
    preset_path = tmp_path / "preset.json"
    eqapo_path = tmp_path / "config.txt"
    _write_wav(wav_path, 48000, _sine(500.0, 48000, duration=2.0))

    _run_cli(["measure", "--input", str(wav_path), "--output", str(preset_path)], monkeypatch)
    _run_cli(["eqapo", "--input", str(wav_path), "--output", str(eqapo_path)], monkeypatch)

    with open(preset_path) as f:
        preset = json.load(f)
    preset_gains = [b["gain_db"] for b in preset["bands"]]
    preset_preamp = preset["preamp_db"]

    text = eqapo_path.read_text(encoding="utf-8")
    eqapo_preamp = float(re.search(r"^Preamp:\s*([-\d.]+)\s*dB\s*$", text, re.MULTILINE).group(1))
    eqapo_gains = [float(g) for g in re.findall(r"Gain\s+([-\d.]+)\s*dB", text)]

    assert eqapo_preamp == pytest.approx(preset_preamp, abs=0.01)
    for a, b in zip(eqapo_gains, preset_gains):
        assert a == pytest.approx(b, abs=0.01)
