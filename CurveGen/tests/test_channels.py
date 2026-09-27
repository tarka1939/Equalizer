"""
tests/test_channels.py — Channel identity, specs, and multichannel containers.

The layout table and acronyms are checked against what Equalizer APO's
configuration reference documents, since a wrong acronym produces a config
that Equalizer APO silently ignores rather than rejecting.
"""
import numpy as np
import pytest

from curvegen.channels import (
    CHANNEL_LAYOUTS,
    KNOWN_ACRONYMS,
    ChannelCurve,
    MultiChannelCurve,
    channel_levels_db,
    label_for,
    layout_for,
    level_offsets_db,
    parse_channel_spec,
)


class TestLayouts:

    def test_stereo_is_l_r(self):
        assert layout_for(2) == ("L", "R")

    def test_mono_is_centre(self):
        """Equalizer APO's table puts a single channel at C, not L."""
        assert layout_for(1) == ("C",)

    def test_five_one_matches_the_documented_order(self):
        assert layout_for(6) == ("L", "R", "C", "LFE", "RL", "RR")

    def test_lfe_not_sub(self):
        """`SUB` is a plausible guess and is not what the docs use."""
        assert "LFE" in KNOWN_ACRONYMS
        assert "SUB" not in KNOWN_ACRONYMS

    def test_unknown_layout_falls_back_to_numeric_positions(self):
        """Equalizer APO accepts 1-based numbers for any channel count."""
        assert layout_for(3) == ("1", "2", "3")
        assert layout_for(0) == ()

    def test_label_for_indexes_within_and_beyond_the_layout(self):
        assert label_for(0, 2) == "L"
        assert label_for(1, 2) == "R"
        assert label_for(5, 2) == "6"          # past the layout: 1-based number


class TestParseChannelSpec:

    def test_comma_and_space_separators_both_work(self):
        assert parse_channel_spec("L,R", 2) == [0, 1]
        assert parse_channel_spec("L R", 2) == [0, 1]

    def test_all_expands_to_every_channel(self):
        assert parse_channel_spec("all", 6) == [0, 1, 2, 3, 4, 5]
        assert parse_channel_spec("ALL", 2) == [0, 1]

    def test_numeric_positions_are_one_based(self):
        """Matching Equalizer APO, where `Channel: 1` is the first channel."""
        assert parse_channel_spec("1 2", 2) == [0, 1]

    def test_mixed_acronyms_and_numbers(self):
        assert parse_channel_spec("1 2 C", 6) == [0, 1, 2]

    def test_order_is_preserved_and_duplicates_collapse(self):
        assert parse_channel_spec("R,L,R", 2) == [1, 0]

    def test_unknown_acronym_is_rejected(self):
        with pytest.raises(ValueError, match="not an Equalizer APO channel position"):
            parse_channel_spec("L,X", 2)

    def test_acronym_absent_from_this_layout_is_rejected(self):
        """`C` is a real position, just not one a stereo file has."""
        with pytest.raises(ValueError, match="not present in a 2-channel layout"):
            parse_channel_spec("C", 2)

    def test_out_of_range_number_is_rejected(self):
        with pytest.raises(ValueError, match="out of range"):
            parse_channel_spec("5", 2)

    def test_empty_spec_is_rejected(self):
        with pytest.raises(ValueError, match="empty channel spec"):
            parse_channel_spec("  ", 2)

    def test_all_cannot_be_combined(self):
        with pytest.raises(ValueError, match="cannot be combined"):
            parse_channel_spec("all,L", 2)


class TestChannelCurve:

    def test_scalar_q_expands_per_filter(self):
        c = ChannelCurve("L", 0, [100.0, 1000.0], [1.0, -2.0], q=1.5)
        assert c.q_list() == [1.5, 1.5]

    def test_per_filter_q_is_kept(self):
        c = ChannelCurve("L", 0, [100.0, 1000.0], [1.0, -2.0], q=[0.7, 3.0])
        assert c.q_list() == [0.7, 3.0]

    def test_mismatched_gain_length_is_rejected(self):
        with pytest.raises(ValueError, match="must match gains_db length"):
            ChannelCurve("L", 0, [100.0, 1000.0], [1.0])

    def test_mismatched_q_length_is_rejected(self):
        with pytest.raises(ValueError, match="q length"):
            ChannelCurve("L", 0, [100.0, 1000.0], [1.0, 2.0], q=[1.0])

    def test_level_offset_defaults_to_zero(self):
        assert ChannelCurve("L", 0, [100.0], [1.0]).level_offset_db == 0.0


class TestMultiChannelCurve:

    def _pair(self, pl=-4.0, pr=-2.0):
        return MultiChannelCurve(n_channels=2, curves=[
            ChannelCurve("L", 0, [100.0], [-3.0], preamp_db=pl),
            ChannelCurve("R", 1, [200.0], [-1.0], preamp_db=pr),
        ])

    def test_shared_preamp_is_the_worst_case(self):
        """One preamp for all channels, sized for the hungriest.

        Using each channel's own preamp would fit tighter but would change one
        speaker's level relative to the other, moving the stereo image. See the
        class docstring.
        """
        assert self._pair(-4.0, -2.0).preamp_db == -4.0
        assert self._pair(-1.0, -7.5).preamp_db == -7.5

    def test_empty_curve_has_no_preamp(self):
        assert MultiChannelCurve().preamp_db == 0.0

    def test_labels_and_coverage(self):
        mc = self._pair()
        assert mc.labels == ["L", "R"]
        assert mc.covers_all_channels()
        assert len(mc) == 2

    def test_partial_coverage_is_reported(self):
        mc = MultiChannelCurve(n_channels=6, curves=[
            ChannelCurve("L", 0, [100.0], [-3.0]),
        ])
        assert not mc.covers_all_channels()


class TestLevelMatching:

    def _measurement(self, level_db, n=1024, sr=48000.0):
        freqs = np.fft.rfftfreq(n, d=1.0 / sr)
        return freqs, np.full_like(freqs, level_db)

    def test_offsets_cut_the_louder_channel_only(self):
        """Never boosts, so matching cannot introduce clipping."""
        left = self._measurement(-2.0)
        right = self._measurement(0.0)
        offsets = level_offsets_db([left, right])
        assert offsets[0] == pytest.approx(0.0)        # already the quietest
        assert offsets[1] == pytest.approx(-2.0)       # cut down to match
        assert all(o <= 1e-9 for o in offsets)

    def test_matched_channels_need_no_trim(self):
        m = self._measurement(-3.0)
        assert level_offsets_db([m, m]) == pytest.approx([0.0, 0.0])

    def test_levels_are_read_at_the_reference_frequency(self):
        left = self._measurement(-6.0)
        assert channel_levels_db([left])[0] == pytest.approx(-6.0)

    def test_empty_input(self):
        assert level_offsets_db([]) == []
        assert channel_levels_db([]) == []

    def test_three_channels_all_referenced_to_the_quietest(self):
        ms = [self._measurement(v) for v in (0.0, -1.0, -4.0)]
        offsets = level_offsets_db(ms)
        assert offsets == pytest.approx([-4.0, -3.0, 0.0])
