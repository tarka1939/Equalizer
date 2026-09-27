"""
tests/test_eqapo_multichannel.py — Per-channel Equalizer APO config output.

The properties pinned here are mostly about *ordering*, because Equalizer APO's
`Channel:` command scopes everything after it: a `Preamp` in the wrong place
attenuates one speaker instead of all of them, and a missing reset at the end
leaks the selection into whatever the user `Include:`s next. Both produce a
config that loads without complaint and does the wrong thing, which is exactly
the sort of defect a format test should catch.
"""
import re

import pytest

from curvegen.channels import ChannelCurve, MultiChannelCurve
from curvegen.eqapo_export import (
    render_eqapo_config,
    render_multichannel_eqapo_config,
    write_multichannel_eqapo_config,
)

CHANNEL_RE = re.compile(r"^Channel:\s+(.+?)\s*$")
PREAMP_RE = re.compile(r"^Preamp:\s+(-?[0-9.]+)\s+dB\s*$")
FILTER_RE = re.compile(
    r"^Filter\s+(\d+):\s+ON\s+PK\s+Fc\s+([0-9.]+)\s+Hz\s+Gain\s+(-?[0-9.]+)\s+dB\s+Q\s+([0-9.]+)\s*$"
)


def commands(text):
    """Every non-comment, non-blank line, in order, as (kind, payload)."""
    out = []
    for line in text.splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        for kind, rx in (("channel", CHANNEL_RE), ("preamp", PREAMP_RE), ("filter", FILTER_RE)):
            m = rx.match(line)
            if m:
                out.append((kind, m.groups()))
                break
        else:
            raise AssertionError(f"unparseable config line: {line!r}")
    return out


def stereo(pl=-4.0, pr=-2.0, offset_l=0.0, offset_r=0.0):
    return MultiChannelCurve(n_channels=2, curves=[
        ChannelCurve("L", 0, [90.0, 1200.0], [-6.0, 2.0], q=[5.0, 1.2],
                     preamp_db=pl, level_offset_db=offset_l),
        ChannelCurve("R", 1, [110.0], [-3.0], q=3.0,
                     preamp_db=pr, level_offset_db=offset_r),
    ])


class TestOrdering:

    def test_global_preamp_precedes_every_channel_block(self):
        """The load-bearing property.

        `Channel:` scopes `Preamp` as well as `Filter`, so the shared preamp
        must appear before the first `Channel:` line. Emitted inside a block it
        would attenuate that one speaker and leave the others clipping.
        """
        cmds = commands(render_multichannel_eqapo_config(stereo()))
        assert cmds[0][0] == "preamp", f"first command was {cmds[0]}"
        assert cmds[0][1][0] == "-4.00"                 # worst case of the two
        first_channel = next(i for i, (k, _) in enumerate(cmds) if k == "channel")
        assert first_channel > 0

    def test_config_ends_by_resetting_the_selection(self):
        """Otherwise the selection leaks past an `Include:` of this file."""
        cmds = commands(render_multichannel_eqapo_config(stereo()))
        assert cmds[-1] == ("channel", ("all",))

    def test_each_channel_block_owns_its_filters(self):
        cmds = commands(render_multichannel_eqapo_config(stereo()))
        current, seen = None, {}
        for kind, payload in cmds:
            if kind == "channel":
                current = payload[0]
                seen.setdefault(current, [])
            elif kind == "filter" and current is not None:
                seen[current].append(payload)
        assert [f[1] for f in seen["L"]] == ["90", "1200"]
        assert [f[1] for f in seen["R"]] == ["110"]
        assert seen["all"] == []

    def test_filter_numbering_restarts_in_each_channel(self):
        """Numbers are cosmetic to Equalizer APO (the docs say they may be
        omitted), and restarting reads better against a per-channel curve."""
        cmds = commands(render_multichannel_eqapo_config(stereo()))
        per_channel, current = {}, None
        for kind, payload in cmds:
            if kind == "channel":
                current = payload[0]
            elif kind == "filter":
                per_channel.setdefault(current, []).append(int(payload[0]))
        assert per_channel["L"] == [1, 2]
        assert per_channel["R"] == [1]


class TestLevelTrim:

    def test_trim_is_emitted_inside_the_channel_block(self):
        cmds = commands(render_multichannel_eqapo_config(stereo(offset_r=-2.37)))
        idx = [i for i, (k, _) in enumerate(cmds) if k == "preamp"]
        assert len(idx) == 2, "expected a global preamp plus one channel trim"
        # The second preamp must sit after `Channel: R` and before the reset.
        r_at = next(i for i, (k, p) in enumerate(cmds) if k == "channel" and p[0] == "R")
        assert idx[1] > r_at
        assert cmds[idx[1]][1][0] == "-2.37"

    def test_no_trim_line_when_channels_already_match(self):
        cmds = commands(render_multichannel_eqapo_config(stereo()))
        assert sum(1 for k, _ in cmds if k == "preamp") == 1

    def test_negligible_trim_is_not_emitted(self):
        """A 0.001 dB trim is noise, and `Preamp: -0.00 dB` reads as a bug."""
        cmds = commands(render_multichannel_eqapo_config(stereo(offset_r=-0.001)))
        assert sum(1 for k, _ in cmds if k == "preamp") == 1


class TestContent:

    def test_per_filter_q_is_preserved(self):
        cmds = commands(render_multichannel_eqapo_config(stereo()))
        qs = [p[3] for k, p in cmds if k == "filter"]
        assert qs == ["5.00", "1.20", "3.00"]

    def test_channel_with_no_filters_still_gets_a_block(self):
        """So the file records that the channel was measured, not skipped."""
        mc = MultiChannelCurve(n_channels=2, curves=[
            ChannelCurve("L", 0, [90.0], [-6.0], preamp_db=-1.0),
            ChannelCurve("R", 1, [], [], preamp_db=0.0),
        ])
        text = render_multichannel_eqapo_config(mc)
        assert "Channel: R" in text
        assert "no filters needed" in text

    def test_comment_lines_are_hash_prefixed(self):
        text = render_multichannel_eqapo_config(stereo(), comment="hello\n\nworld")
        assert "# hello" in text and "# world" in text
        commands(text)          # must still parse: comments are ignorable

    def test_empty_curve_is_rejected(self):
        with pytest.raises(ValueError, match="no channel curves"):
            render_multichannel_eqapo_config(MultiChannelCurve())

    def test_more_than_two_channels(self):
        mc = MultiChannelCurve(n_channels=6, curves=[
            ChannelCurve(lbl, i, [100.0], [-1.0], preamp_db=-1.0)
            for i, lbl in enumerate(("L", "R", "C", "LFE", "RL", "RR"))
        ])
        labels = [p[0] for k, p in commands(render_multichannel_eqapo_config(mc))
                  if k == "channel"]
        assert labels == ["L", "R", "C", "LFE", "RL", "RR", "all"]


class TestSingleChannelUnaffected:

    def test_plain_renderer_emits_no_channel_command(self):
        """Mono configs must not sprout `Channel:` lines: an existing config
        that never mentioned channels should keep applying to all of them."""
        text = render_eqapo_config([1.0] * 10)
        assert "Channel:" not in text


class TestWriter:

    def test_writes_file_and_creates_parents(self, tmp_path):
        out = tmp_path / "nested" / "mc.txt"
        write_multichannel_eqapo_config(str(out), stereo(), comment="x")
        assert out.exists()
        assert "Channel: L" in out.read_text(encoding="utf-8")
