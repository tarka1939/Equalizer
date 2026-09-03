"""
tests/test_cli_channels.py — Per-channel correction through the real CLI.

Drives `eq-curvegen eqapo` end to end against synthetic impulse responses whose
defects are known in closed form, one distinct room per channel, so the emitted
config can be checked for *the right filter landing on the right speaker* --
not merely for being well-formed. A per-channel feature that silently applied
the left speaker's curve to both would pass every format check.
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

CHANNEL_RE = re.compile(r"^Channel:\s+(.+?)\s*$")
PREAMP_RE = re.compile(r"^Preamp:\s+(-?[0-9.]+)\s+dB\s*$")
FILTER_RE = re.compile(
    r"^Filter\s+\d+:\s+ON\s+PK\s+Fc\s+([0-9.]+)\s+Hz\s+Gain\s+(-?[0-9.]+)\s+dB\s+Q\s+([0-9.]+)\s*$"
)

# Two clearly different rooms, one per speaker: a low boom on the left and a
# presence-region dip on the right. Nothing in common, so a curve applied to
# the wrong channel is obvious.
LEFT_DEFECT = (70.0, 5.0, 9.0)
RIGHT_DEFECT = (3000.0, 2.0, -7.0)


def _peaking(fc, q, gain_db, sr=SR):
    A = 10 ** (gain_db / 40)
    w = 2 * np.pi * fc / sr
    al = np.sin(w) / (2 * q)
    c = np.cos(w)
    b = np.array([1 + al * A, -2 * c, 1 - al * A])
    a = np.array([1 + al / A, -2 * c, 1 - al / A])
    return b / a[0], a / a[0]


def _ir(defects, level_db=0.0):
    ir = np.zeros(N)
    ir[0] = 1.0
    for fc, q, g in defects:
        b, a = _peaking(fc, q, g)
        ir = sg.lfilter(b, a, ir)
    ir = np.concatenate([np.zeros(120), ir])[:N]
    return ir / np.max(np.abs(ir)) * (10 ** (level_db / 20.0))


@pytest.fixture
def rooms(tmp_path):
    """Left/right IRs as separate files and as one stereo file.

    The right channel is written 2 dB hotter so level matching has something
    real to find.
    """
    left = _ir([LEFT_DEFECT], level_db=-2.0)
    right = _ir([RIGHT_DEFECT], level_db=0.0)
    paths = {
        "L": tmp_path / "ir_L.wav",
        "R": tmp_path / "ir_R.wav",
        "stereo": tmp_path / "ir_stereo.wav",
    }
    wavfile.write(str(paths["L"]), SR, left.astype(np.float32))
    wavfile.write(str(paths["R"]), SR, right.astype(np.float32))
    wavfile.write(str(paths["stereo"]), SR,
                  np.column_stack([left, right]).astype(np.float32))
    return paths


def _run(argv, monkeypatch):
    monkeypatch.setattr(sys, "argv", ["eq-curvegen"] + argv)
    try:
        cli.main()
    except SystemExit as e:
        return e.code if e.code is not None else 0
    return 0


def _blocks(text):
    """{channel label: {"preamp": float|None, "filters": [(fc, gain, q)]}}, in order."""
    out, current, global_preamp = {}, None, None
    for line in text.splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        m = CHANNEL_RE.match(line)
        if m:
            current = m.group(1)
            out.setdefault(current, {"preamp": None, "filters": []})
            continue
        m = PREAMP_RE.match(line)
        if m:
            if current is None:
                global_preamp = float(m.group(1))
            else:
                out[current]["preamp"] = float(m.group(1))
            continue
        m = FILTER_RE.match(line)
        assert m, f"unparseable line: {line!r}"
        assert current is not None, "a Filter appeared before any Channel:"
        out[current]["filters"].append(
            (float(m.group(1)), float(m.group(2)), float(m.group(3))))
    return global_preamp, out


def _strongest(filters):
    return max(filters, key=lambda f: abs(f[1]))


# ── One file per channel: the shape real measurements take ────────────────────

class TestSeparateFilesPerChannel:

    def test_each_speaker_gets_its_own_correction(self, rooms, tmp_path, monkeypatch):
        out = tmp_path / "mc.txt"
        assert _run(["eqapo",
                     "--channel-input", f"L={rooms['L']}",
                     "--channel-input", f"R={rooms['R']}",
                     "--ir", "--mode", "parametric", "--output", str(out)], monkeypatch) == 0

        _, blocks = _blocks(out.read_text(encoding="utf-8"))
        assert list(blocks) == ["L", "R", "all"]

        lf = _strongest(blocks["L"]["filters"])
        assert 60.0 < lf[0] < 85.0, f"left filter landed at {lf[0]} Hz, not on the 70 Hz mode"
        assert lf[1] < 0.0, "the left room has a boost; the fix is a cut"

        rf = _strongest(blocks["R"]["filters"])
        assert 2500.0 < rf[0] < 3600.0, f"right filter landed at {rf[0]} Hz, not on the 3 kHz dip"
        assert rf[1] > 0.0, "the right room has a dip; the fix is a boost"

    def test_the_two_channels_do_not_get_the_same_curve(self, rooms, tmp_path, monkeypatch):
        """Guards the failure this feature is most likely to have: measuring
        once and writing the same curve into every Channel block."""
        out = tmp_path / "mc.txt"
        _run(["eqapo", "--channel-input", f"L={rooms['L']}",
              "--channel-input", f"R={rooms['R']}",
              "--ir", "--mode", "parametric", "--output", str(out)], monkeypatch)
        _, blocks = _blocks(out.read_text(encoding="utf-8"))
        assert blocks["L"]["filters"] != blocks["R"]["filters"]

    def test_graphic_mode_gives_ten_bands_per_channel(self, rooms, tmp_path, monkeypatch):
        out = tmp_path / "mc.txt"
        assert _run(["eqapo", "--channel-input", f"L={rooms['L']}",
                     "--channel-input", f"R={rooms['R']}",
                     "--ir", "--output", str(out)], monkeypatch) == 0
        _, blocks = _blocks(out.read_text(encoding="utf-8"))
        assert len(blocks["L"]["filters"]) == 10
        assert len(blocks["R"]["filters"]) == 10
        assert {f[2] for f in blocks["L"]["filters"]} == {1.0}      # shared Q

    def test_shared_preamp_is_the_worst_case_of_the_channels(self, rooms, tmp_path, monkeypatch):
        out = tmp_path / "mc.txt"
        _run(["eqapo", "--channel-input", f"L={rooms['L']}",
              "--channel-input", f"R={rooms['R']}",
              "--ir", "--mode", "parametric", "--output", str(out)], monkeypatch)
        global_preamp, blocks = _blocks(out.read_text(encoding="utf-8"))
        assert global_preamp is not None and global_preamp <= 0.0
        # It precedes the first Channel:, which _blocks enforces by putting it
        # in `global_preamp` rather than in a block.


# ── Several channels of one file ──────────────────────────────────────────────

class TestChannelsOfOneFile:

    def test_stereo_file_split_into_two_blocks(self, rooms, tmp_path, monkeypatch):
        out = tmp_path / "mc.txt"
        assert _run(["eqapo", "--input", str(rooms["stereo"]), "--ir",
                     "--channels", "L,R", "--mode", "parametric",
                     "--output", str(out)], monkeypatch) == 0
        _, blocks = _blocks(out.read_text(encoding="utf-8"))
        assert list(blocks) == ["L", "R", "all"]
        assert 60.0 < _strongest(blocks["L"]["filters"])[0] < 85.0
        assert 2500.0 < _strongest(blocks["R"]["filters"])[0] < 3600.0

    def test_all_is_accepted_for_a_stereo_file(self, rooms, tmp_path, monkeypatch):
        out = tmp_path / "mc.txt"
        assert _run(["eqapo", "--input", str(rooms["stereo"]), "--ir",
                     "--channels", "all", "--output", str(out)], monkeypatch) == 0
        _, blocks = _blocks(out.read_text(encoding="utf-8"))
        assert list(blocks) == ["L", "R", "all"]

    def test_a_single_channel_still_uses_the_channel_form(self, rooms, tmp_path, monkeypatch):
        """`--channels R` is a legitimate way to correct one speaker."""
        out = tmp_path / "mc.txt"
        assert _run(["eqapo", "--input", str(rooms["stereo"]), "--ir",
                     "--channels", "R", "--output", str(out)], monkeypatch) == 0
        _, blocks = _blocks(out.read_text(encoding="utf-8"))
        assert list(blocks) == ["R", "all"]


# ── Level matching ────────────────────────────────────────────────────────────

class TestLevelMatching:

    def test_off_by_default(self, rooms, tmp_path, monkeypatch):
        out = tmp_path / "mc.txt"
        _run(["eqapo", "--channel-input", f"L={rooms['L']}",
              "--channel-input", f"R={rooms['R']}",
              "--ir", "--mode", "parametric", "--output", str(out)], monkeypatch)
        _, blocks = _blocks(out.read_text(encoding="utf-8"))
        assert blocks["L"]["preamp"] is None
        assert blocks["R"]["preamp"] is None

    def test_trims_the_louder_channel_only(self, rooms, tmp_path, monkeypatch):
        """The right IR is written 2 dB hotter, so it should be cut ~2 dB and
        the left left alone -- matching never boosts."""
        out = tmp_path / "mc.txt"
        assert _run(["eqapo", "--channel-input", f"L={rooms['L']}",
                     "--channel-input", f"R={rooms['R']}",
                     "--ir", "--mode", "parametric", "--match-channels",
                     "--output", str(out)], monkeypatch) == 0
        _, blocks = _blocks(out.read_text(encoding="utf-8"))
        assert blocks["L"]["preamp"] is None, "the quieter channel needs no trim"
        assert blocks["R"]["preamp"] is not None
        assert -3.5 < blocks["R"]["preamp"] < -1.5, (
            f"expected roughly -2 dB on the louder channel, got {blocks['R']['preamp']}")

    def test_matching_does_not_change_the_filters(self, rooms, tmp_path, monkeypatch):
        """Level is a gain, so it must not leak into the filter solve."""
        plain, matched = tmp_path / "a.txt", tmp_path / "b.txt"
        base = ["eqapo", "--channel-input", f"L={rooms['L']}",
                "--channel-input", f"R={rooms['R']}", "--ir", "--mode", "parametric"]
        _run(base + ["--output", str(plain)], monkeypatch)
        _run(base + ["--match-channels", "--output", str(matched)], monkeypatch)
        _, a = _blocks(plain.read_text(encoding="utf-8"))
        _, b = _blocks(matched.read_text(encoding="utf-8"))
        assert a["L"]["filters"] == b["L"]["filters"]
        assert a["R"]["filters"] == b["R"]["filters"]


# ── Argument handling ─────────────────────────────────────────────────────────

class TestArgumentErrors:

    def test_input_and_channel_input_together_are_rejected(self, rooms, tmp_path, monkeypatch):
        assert _run(["eqapo", "--input", str(rooms["stereo"]),
                     "--channel-input", f"L={rooms['L']}",
                     "--output", str(tmp_path / "x.txt")], monkeypatch) == 2

    def test_channels_without_input_is_rejected(self, tmp_path, monkeypatch):
        assert _run(["eqapo", "--channels", "L,R",
                     "--output", str(tmp_path / "x.txt")], monkeypatch) == 2

    def test_neither_input_nor_channel_input_is_rejected(self, tmp_path, monkeypatch):
        assert _run(["eqapo", "--output", str(tmp_path / "x.txt")], monkeypatch) == 2

    def test_channel_absent_from_the_layout_is_rejected(self, rooms, tmp_path, monkeypatch):
        assert _run(["eqapo", "--input", str(rooms["stereo"]), "--ir",
                     "--channels", "L,R,C",
                     "--output", str(tmp_path / "x.txt")], monkeypatch) == 2

    def test_malformed_channel_input_is_rejected(self, rooms, tmp_path, monkeypatch):
        assert _run(["eqapo", "--channel-input", "bogus", "--ir",
                     "--output", str(tmp_path / "x.txt")], monkeypatch) == 2

    def test_unknown_channel_label_is_rejected(self, rooms, tmp_path, monkeypatch):
        assert _run(["eqapo", "--channel-input", f"Z={rooms['L']}", "--ir",
                     "--output", str(tmp_path / "x.txt")], monkeypatch) == 2


class TestSingleChannelUnchanged:

    def test_plain_eqapo_emits_no_channel_command(self, rooms, tmp_path, monkeypatch):
        """Existing single-channel behaviour must be untouched -- a config with
        no `Channel:` line applies to every channel, which is what users of the
        previous version already have."""
        out = tmp_path / "single.txt"
        assert _run(["eqapo", "--input", str(rooms["L"]), "--ir",
                     "--output", str(out)], monkeypatch) == 0
        assert "Channel:" not in out.read_text(encoding="utf-8")
