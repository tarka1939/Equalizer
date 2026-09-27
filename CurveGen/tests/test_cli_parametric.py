"""
tests/test_cli_parametric.py — Integration tests for `eq-curvegen eqapo --mode parametric`.

Exercises the real argparse wiring and the full
measurement -> parametric -> eqapo_export pipeline against a synthetic impulse
response whose response is known in closed form, so the emitted config can be
checked against the right answer rather than merely for well-formedness.

`--mode parametric` is only offered on `eqapo`. That is a deliberate limit, not
an oversight: `shared/preset_schema.json` pins `bands` to exactly 10 entries and
`DSP::Equalizer10Band` has a fixed band count and a single shared Q, so a curve
with a solver-chosen filter count and per-filter Q has nowhere else to go today.
See the module docstring in `curvegen/parametric.py`.
"""
from __future__ import annotations

import re
import sys

import numpy as np
import pytest
import scipy.signal as sg
from scipy.io import wavfile

from curvegen import cli

SR = 48000
N = 16384

FILTER_RE = re.compile(
    r"^Filter\s+(\d+):\s+ON\s+PK\s+Fc\s+([0-9.]+)\s+Hz\s+Gain\s+(-?[0-9.]+)\s+dB\s+Q\s+([0-9.]+)\s*$"
)
PREAMP_RE = re.compile(r"^Preamp:\s+(-?[0-9.]+)\s+dB\s*$")


def _peaking(fc, q, gain_db, sr=SR):
    A = 10 ** (gain_db / 40)
    w = 2 * np.pi * fc / sr
    al = np.sin(w) / (2 * q)
    c = np.cos(w)
    b = np.array([1 + al * A, -2 * c, 1 - al * A])
    a = np.array([1 + al / A, -2 * c, 1 - al / A])
    return b / a[0], a / a[0]


def _write_ir(path, defects):
    """An impulse response for a room built from known peaking filters."""
    ir = np.zeros(N)
    ir[0] = 1.0
    for fc, q, g in defects:
        b, a = _peaking(fc, q, g)
        ir = sg.lfilter(b, a, ir)
    ir = np.concatenate([np.zeros(120), ir])[:N]      # peak a little way in, like a real export
    ir /= np.max(np.abs(ir))
    wavfile.write(str(path), SR, ir.astype(np.float32))


def _run_cli(argv, monkeypatch):
    monkeypatch.setattr(sys, "argv", ["eq-curvegen"] + argv)
    try:
        cli.main()
    except SystemExit as e:
        return e.code if e.code is not None else 0
    return 0


def _parse(text):
    preamp, filters = None, []
    for line in text.splitlines():
        if line.startswith("#") or not line.strip():
            continue
        m = PREAMP_RE.match(line)
        if m:
            preamp = float(m.group(1))
            continue
        m = FILTER_RE.match(line)
        assert m, f"unparseable config line: {line!r}"
        filters.append({
            "n": int(m.group(1)),
            "fc": float(m.group(2)),
            "gain": float(m.group(3)),
            "q": float(m.group(4)),
        })
    return preamp, filters


# ── Tests ─────────────────────────────────────────────────────────────────────

def test_parametric_mode_recovers_a_known_room(tmp_path, monkeypatch):
    """The Phase 2 room: +8 dB at 125 Hz Q1, -6 dB at 2 kHz Q1.

    Both defects are peaking filters, so the exactly-correct answer is two
    filters at those frequencies with the opposite gains -- and unlike the
    fixed grid, the parametric solver is able to produce exactly that.
    """
    ir = tmp_path / "synthetic_ir.wav"
    out = tmp_path / "parametric.txt"
    _write_ir(ir, [(125.0, 1.0, 8.0), (2000.0, 1.0, -6.0)])

    code = _run_cli(
        ["eqapo", "--input", str(ir), "--ir", "--mode", "parametric", "--output", str(out)],
        monkeypatch,
    )
    assert code == 0
    preamp, filters = _parse(out.read_text(encoding="utf-8"))

    assert preamp is not None and preamp <= 0.0
    assert len(filters) == 2, f"expected 2 filters, got {[f['fc'] for f in filters]}"

    low, high = filters[0], filters[1]
    assert 115.0 < low["fc"] < 135.0
    assert -9.0 < low["gain"] < -7.0
    assert 0.8 < low["q"] < 1.3

    assert 1850.0 < high["fc"] < 2150.0
    assert 5.0 < high["gain"] < 7.0
    assert 0.8 < high["q"] < 1.3


def test_parametric_places_a_narrow_filter_on_a_narrow_mode(tmp_path, monkeypatch):
    """The case the fixed grid cannot express: +10 dB at 90 Hz, Q 6.

    90 Hz is not an ISO band centre and Q 6 is six times narrower than the
    graphic solver's fixed Q.
    """
    ir = tmp_path / "mode.wav"
    out = tmp_path / "mode.txt"
    _write_ir(ir, [(90.0, 6.0, 10.0)])

    assert _run_cli(
        ["eqapo", "--input", str(ir), "--ir", "--mode", "parametric", "--output", str(out)],
        monkeypatch,
    ) == 0
    _, filters = _parse(out.read_text(encoding="utf-8"))

    strongest = max(filters, key=lambda f: abs(f["gain"]))
    assert 80.0 < strongest["fc"] < 100.0, f"filter landed at {strongest['fc']} Hz"
    assert strongest["gain"] < 0.0
    assert strongest["q"] > 3.0, f"expected a narrow filter, got Q {strongest['q']}"


def test_graphic_mode_is_the_default_and_unchanged(tmp_path, monkeypatch):
    """Issue #4's third acceptance criterion: existing behaviour is the default.

    Ten filters, all on ISO centres, all at the shared Q -- byte-for-byte the
    behaviour that existed before parametric mode was added.
    """
    ir = tmp_path / "room.wav"
    default_out = tmp_path / "default.txt"
    explicit_out = tmp_path / "explicit.txt"
    _write_ir(ir, [(125.0, 1.0, 8.0), (2000.0, 1.0, -6.0)])

    assert _run_cli(["eqapo", "--input", str(ir), "--ir",
                     "--output", str(default_out)], monkeypatch) == 0
    assert _run_cli(["eqapo", "--input", str(ir), "--ir", "--mode", "graphic",
                     "--output", str(explicit_out)], monkeypatch) == 0

    assert default_out.read_text(encoding="utf-8") == explicit_out.read_text(encoding="utf-8")

    _, filters = _parse(default_out.read_text(encoding="utf-8"))
    assert len(filters) == 10
    assert [f["fc"] for f in filters] == [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]
    assert {f["q"] for f in filters} == {1.0}


def test_filter_budget_is_respected(tmp_path, monkeypatch):
    ir = tmp_path / "room.wav"
    out = tmp_path / "few.txt"
    _write_ir(ir, [(58.0, 5.0, 9.0), (140.0, 2.0, -5.0), (3200.0, 1.5, 4.0)])

    assert _run_cli(["eqapo", "--input", str(ir), "--ir", "--mode", "parametric",
                     "--filters", "2", "--output", str(out)], monkeypatch) == 0
    _, filters = _parse(out.read_text(encoding="utf-8"))
    assert 1 <= len(filters) <= 2


def test_q_and_frequency_ranges_are_respected(tmp_path, monkeypatch):
    ir = tmp_path / "room.wav"
    out = tmp_path / "bounded.txt"
    _write_ir(ir, [(90.0, 6.0, 10.0), (5000.0, 3.0, -8.0)])

    assert _run_cli(["eqapo", "--input", str(ir), "--ir", "--mode", "parametric",
                     "--q-range", "1.0", "3.0",
                     "--freq-range", "60", "2000",
                     "--max-gain", "6",
                     "--output", str(out)], monkeypatch) == 0
    _, filters = _parse(out.read_text(encoding="utf-8"))
    assert filters, "expected at least one filter"
    for f in filters:
        assert 1.0 - 0.01 <= f["q"] <= 3.0 + 0.01, f"Q {f['q']} outside the requested range"
        assert 60.0 - 1.0 <= f["fc"] <= 2000.0 + 1.0, f"Fc {f['fc']} outside the requested range"
        assert abs(f["gain"]) <= 6.0 + 0.01


def test_parametric_config_carries_per_filter_q(tmp_path, monkeypatch):
    """The point of the format: Q varies between filters rather than being shared."""
    ir = tmp_path / "room.wav"
    out = tmp_path / "varied.txt"
    _write_ir(ir, [(70.0, 8.0, 9.0), (900.0, 0.9, -6.0)])

    assert _run_cli(["eqapo", "--input", str(ir), "--ir", "--mode", "parametric",
                     "--output", str(out)], monkeypatch) == 0
    _, filters = _parse(out.read_text(encoding="utf-8"))
    qs = {f["q"] for f in filters}
    assert len(qs) > 1, f"expected differing Q values across filters, got {qs}"
