"""
tests/test_eqapo_export.py — Unit tests for eqapo_export.py.

The Equalizer APO config format (Preamp:/Filter: lines) is documented at
https://sourceforge.net/p/equalizerapo/wiki/Configuration%20reference/ --
these tests parse the *generated* text with a small regex-based reader (not
part of the production module; Equalizer APO itself is the real consumer)
to confirm the values round-trip correctly, rather than only checking
substring presence.
"""
import os
import re

import pytest

from curvegen.eqapo_export import (
    DEFAULT_Q,
    render_eqapo_config,
    write_eqapo_config,
)

SAMPLE_BAND_HZ = [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]
SAMPLE_GAINS = [2.5, 1.0, -1.5, 0.0, 0.5, -2.0, 1.0, 0.0, -0.5, 1.5]
SAMPLE_PREAMP = -3.0

_PREAMP_RE = re.compile(r"^Preamp:\s*([-\d.]+)\s*dB\s*$", re.IGNORECASE)
_FILTER_RE = re.compile(
    r"^Filter\s+(\d+):\s*ON\s+PK\s+Fc\s+([-\d.]+)\s*Hz\s+"
    r"Gain\s+([-\d.]+)\s*dB\s+Q\s+([-\d.]+)\s*$",
    re.IGNORECASE,
)


def _parse(text: str):
    """Minimal test-only reader: pulls out (preamp_db, [(n, fc, gain, q), ...])."""
    preamp = None
    filters = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = _PREAMP_RE.match(line)
        if m:
            preamp = float(m.group(1))
            continue
        m = _FILTER_RE.match(line)
        if m:
            n, fc, gain, q = m.groups()
            filters.append((int(n), float(fc), float(gain), float(q)))
    return preamp, filters


class TestRenderEqapoConfig:

    def test_preamp_and_filters_round_trip(self):
        text = render_eqapo_config(SAMPLE_GAINS, SAMPLE_BAND_HZ, SAMPLE_PREAMP)
        preamp, filters = _parse(text)

        assert preamp == pytest.approx(SAMPLE_PREAMP, abs=0.01)
        assert len(filters) == len(SAMPLE_BAND_HZ)

        for (n, fc, gain, q), hz, expected_gain in zip(filters, SAMPLE_BAND_HZ, SAMPLE_GAINS):
            assert fc == pytest.approx(hz, abs=0.01)
            assert gain == pytest.approx(expected_gain, abs=0.01)
            assert q == pytest.approx(DEFAULT_Q, abs=0.01)

    def test_filter_numbers_are_1_indexed_and_sequential(self):
        text = render_eqapo_config(SAMPLE_GAINS, SAMPLE_BAND_HZ, SAMPLE_PREAMP)
        _, filters = _parse(text)
        assert [n for n, *_ in filters] == list(range(1, len(SAMPLE_BAND_HZ) + 1))

    def test_uses_pk_peaking_filter_type(self):
        # Not just "some filter got written" -- specifically the peaking
        # type, since that's what makes this a faithful match for
        # DSP::Biquad's RBJ peaking filter (see module docstring).
        text = render_eqapo_config(SAMPLE_GAINS, SAMPLE_BAND_HZ, SAMPLE_PREAMP)
        for line in text.splitlines():
            if line.startswith("Filter"):
                assert "ON PK" in line

    def test_default_band_hz_used_when_omitted(self):
        from curvegen.flatten import DEFAULT_BAND_HZ
        text = render_eqapo_config(SAMPLE_GAINS)
        _, filters = _parse(text)
        for (_, fc, _, _), hz in zip(filters, DEFAULT_BAND_HZ):
            assert fc == pytest.approx(hz, abs=0.01)

    def test_scalar_q_applied_to_every_band(self):
        text = render_eqapo_config(SAMPLE_GAINS, SAMPLE_BAND_HZ, 0.0, q=2.5)
        _, filters = _parse(text)
        for _, _, _, q in filters:
            assert q == pytest.approx(2.5, abs=0.01)

    def test_per_band_q_sequence(self):
        q_values = [round(1.0 + 0.1 * i, 2) for i in range(len(SAMPLE_BAND_HZ))]
        text = render_eqapo_config(SAMPLE_GAINS, SAMPLE_BAND_HZ, 0.0, q=q_values)
        _, filters = _parse(text)
        for (_, _, _, q), expected in zip(filters, q_values):
            assert q == pytest.approx(expected, abs=0.01)

    def test_mismatched_q_length_raises(self):
        with pytest.raises(ValueError, match="q length"):
            render_eqapo_config(SAMPLE_GAINS, SAMPLE_BAND_HZ, 0.0, q=[1.0, 2.0])

    def test_non_positive_q_raises(self):
        with pytest.raises(ValueError, match="Q must be strictly positive"):
            render_eqapo_config(SAMPLE_GAINS, SAMPLE_BAND_HZ, 0.0, q=0.0)

    def test_mismatched_band_gain_lengths_raises(self):
        with pytest.raises(ValueError, match="length"):
            render_eqapo_config(SAMPLE_GAINS[:5], SAMPLE_BAND_HZ, 0.0)

    def test_comment_lines_are_hash_prefixed_and_ignorable(self):
        text = render_eqapo_config(
            SAMPLE_GAINS, SAMPLE_BAND_HZ, SAMPLE_PREAMP,
            comment="line one\nline two",
        )
        comment_lines = [l for l in text.splitlines() if l.startswith("#")]
        assert comment_lines == ["# line one", "# line two"]
        # And the parser (which skips '#' lines, matching Equalizer APO's
        # own "unsupported lines are silently ignored" behavior) still
        # reads the real content correctly.
        preamp, filters = _parse(text)
        assert preamp == pytest.approx(SAMPLE_PREAMP, abs=0.01)
        assert len(filters) == len(SAMPLE_BAND_HZ)

    def test_negative_and_zero_gains_format_correctly(self):
        gains = [-12.0, 0.0, 12.0]
        bands = [100, 1000, 10000]
        text = render_eqapo_config(gains, bands, 0.0)
        _, filters = _parse(text)
        assert [gain for _, _, gain, _ in filters] == [-12.0, 0.0, 12.0]

    def test_whole_number_frequency_has_no_trailing_decimal(self):
        text = render_eqapo_config([0.0], [1000.0], 0.0)
        assert "Fc 1000 Hz" in text

    def test_fractional_frequency_is_preserved(self):
        text = render_eqapo_config([0.0], [31.25], 0.0)
        assert "Fc 31.25 Hz" in text


class TestWriteEqapoConfig:

    def test_writes_file(self, tmp_path):
        path = str(tmp_path / "eqapo_config.txt")
        write_eqapo_config(path, SAMPLE_GAINS, SAMPLE_BAND_HZ, SAMPLE_PREAMP)
        assert os.path.exists(path)
        with open(path, encoding="utf-8") as f:
            text = f.read()
        preamp, filters = _parse(text)
        assert preamp == pytest.approx(SAMPLE_PREAMP, abs=0.01)
        assert len(filters) == len(SAMPLE_BAND_HZ)

    def test_creates_parent_dir(self, tmp_path):
        path = str(tmp_path / "subdir" / "config.txt")
        write_eqapo_config(path, SAMPLE_GAINS, SAMPLE_BAND_HZ, 0.0)
        assert os.path.exists(path)
