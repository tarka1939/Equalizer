"""
channels.py — Channel identity and multichannel correction curves.

Room correction is per-speaker. A left speaker in a corner and a right speaker
beside a doorway do not have the same response, and a single curve derived from
one measurement (or from an average) corrects neither of them properly. This
module carries the channel bookkeeping that lets each speaker get its own
curve, and the container the exporters consume.

Channel naming follows Equalizer APO's documented `Channel:` command, verified
against the official configuration reference rather than assumed:
https://sourceforge.net/p/equalizerapo/wiki/Configuration%20reference/

    Channel: <position 1> <position 2> ...

Positions are the acronyms below, 1-based numeric indices, or the special
`all`. The acronym-to-index mapping depends on the channel count, which is why
`layout_for()` takes one:

    | layout | 1 | 2 | 3 | 4   | 5  | 6  |
    |--------|---|---|---|-----|----|----|
    | mono   | C |   |   |     |    |    |
    | stereo | L | R |   |     |    |    |
    | 5.1    | L | R | C | LFE | RL | RR |

Note the LFE acronym is `LFE`, not `SUB`, and there is an `RC` (rear centre)
in layouts that have one. Anything outside these layouts falls back to 1-based
numeric positions, which Equalizer APO accepts for any channel count.

**A correction that this module deliberately does not apply: channel level
imbalance.** Each channel's target curve is referenced to its own 1 kHz level
(see `flatten._target_curve`), so if the left speaker measures 2 dB quieter
than the right overall, that difference survives correction untouched.

That is the safe default rather than the obviously-right one. A single
microphone is rarely equidistant from both speakers, so a measured level
difference is at least as likely to be the mic's position as the speakers'
gain, and "correcting" it would drag the stereo image sideways permanently.
`level_offsets_db()` exists for callers who know their measurement geometry is
trustworthy and do want the imbalance corrected; the CLI exposes it as
`--match-channels`.

**Level matching is a gain, not a filter**, and this is worth being explicit
about because the obvious implementation does not work. Referencing each
channel's *target curve* to a shared level instead of its own cannot express
an imbalance: `flatten._target_curve` is invariant to a constant offset in the
input, and `parametric.solve_parametric_filters` deliberately projects any
constant out of its residual (a constant dB offset is volume, which no peaking
filter can produce). A level difference between speakers is exactly such a
constant. It therefore belongs in a per-channel `Preamp` line -- which is what
`ChannelCurve.level_offset_db` carries -- and never in the filters.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Sequence, Tuple, Union

import numpy as np

# ── Channel layouts ───────────────────────────────────────────────────────────

#: Acronym per 0-based index, keyed by channel count. Mirrors the table in
#: Equalizer APO's configuration reference.
CHANNEL_LAYOUTS: Dict[int, Tuple[str, ...]] = {
    1: ("C",),
    2: ("L", "R"),
    4: ("L", "R", "RL", "RR"),
    6: ("L", "R", "C", "LFE", "RL", "RR"),
    8: ("L", "R", "C", "LFE", "RL", "RR", "SL", "SR"),
}

#: Every acronym Equalizer APO documents, for validating a user-supplied spec
#: even when the layout in use does not contain that position.
KNOWN_ACRONYMS: Tuple[str, ...] = ("L", "R", "C", "LFE", "RL", "RR", "RC", "SL", "SR")


def layout_for(n_channels: int) -> Tuple[str, ...]:
    """Acronyms for a channel count, or 1-based numeric labels if unknown."""
    if n_channels in CHANNEL_LAYOUTS:
        return CHANNEL_LAYOUTS[n_channels]
    return tuple(str(i + 1) for i in range(max(int(n_channels), 0)))


def label_for(index: int, n_channels: int) -> str:
    """Equalizer APO position name for a 0-based channel index."""
    layout = layout_for(n_channels)
    if 0 <= index < len(layout):
        return layout[index]
    return str(index + 1)


def parse_channel_spec(spec: str, n_channels: int) -> List[int]:
    """
    Resolve a channel spec to 0-based indices.

    Accepts acronyms (`L`, `RL`, ...), 1-based numeric positions as Equalizer
    APO writes them, and `all`. Separators may be commas or whitespace, so both
    `L,R` and `"L R"` work -- the former is what a shell wants, the latter is
    what the config file itself uses.

    Raises ValueError on an unknown name or an out-of-range position, rather
    than silently dropping it: a typo'd channel would otherwise produce a
    config that is short one speaker with no indication why.
    """
    layout = layout_for(n_channels)
    tokens = [t for t in spec.replace(",", " ").split() if t]
    if not tokens:
        raise ValueError("empty channel spec")

    if len(tokens) == 1 and tokens[0].lower() == "all":
        return list(range(n_channels))

    out: List[int] = []
    for tok in tokens:
        if tok.lower() == "all":
            raise ValueError("'all' cannot be combined with other channel positions")

        if tok.isdigit():
            pos = int(tok)
            if not (1 <= pos <= n_channels):
                raise ValueError(
                    f"channel position {pos} is out of range for {n_channels} "
                    f"channel(s) (positions are 1-based)")
            idx = pos - 1
        else:
            upper = tok.upper()
            if upper not in layout:
                known = ", ".join(layout)
                extra = ("" if upper in KNOWN_ACRONYMS else
                         f" '{tok}' is not an Equalizer APO channel position.")
                raise ValueError(
                    f"channel '{tok}' is not present in a {n_channels}-channel "
                    f"layout (available: {known}).{extra}")
            idx = layout.index(upper)

        if idx not in out:
            out.append(idx)

    return out


# ── Curve containers ──────────────────────────────────────────────────────────

@dataclass
class ChannelCurve:
    """One channel's correction, in the shape both exporters already speak.

    `band_hz`, `gains_db` and `q` are parallel per-filter arrays -- `q` may be
    a scalar shared by every filter (graphic mode) or one value per filter
    (parametric mode), matching what `eqapo_export` and `response` accept.

    `preamp_db` is the headroom *this channel alone* would need. It is kept
    per channel for reporting, but see `MultiChannelCurve.preamp_db` for what
    actually gets applied and why.
    """

    label: str
    index: int
    band_hz: np.ndarray
    gains_db: np.ndarray
    q: Union[float, np.ndarray] = 1.0
    preamp_db: float = 0.0
    #: Per-channel level trim in dB, for correcting inter-channel imbalance.
    #: Always <= 0 as produced by `level_offsets_db` (louder channels are cut
    #: down to the quietest rather than quiet ones boosted, so matching never
    #: costs headroom). Zero unless the caller opted into level matching.
    level_offset_db: float = 0.0

    def __post_init__(self) -> None:
        self.band_hz = np.asarray(self.band_hz, dtype=float)
        self.gains_db = np.asarray(self.gains_db, dtype=float)
        if len(self.band_hz) != len(self.gains_db):
            raise ValueError(
                f"channel {self.label}: band_hz length ({len(self.band_hz)}) "
                f"must match gains_db length ({len(self.gains_db)})")
        if not isinstance(self.q, (int, float)):
            self.q = np.asarray(self.q, dtype=float)
            if len(self.q) != len(self.band_hz):
                raise ValueError(
                    f"channel {self.label}: q length ({len(self.q)}) must match "
                    f"band_hz length ({len(self.band_hz)})")

    def __len__(self) -> int:
        return len(self.band_hz)

    def q_list(self) -> List[float]:
        """Per-filter Q, expanded from a scalar if necessary."""
        if isinstance(self.q, (int, float)):
            return [float(self.q)] * len(self.band_hz)
        return [float(v) for v in self.q]


@dataclass
class MultiChannelCurve:
    """A correction for one or more channels, plus the preamp to apply.

    **The preamp is shared across channels on purpose.** Each channel's curve
    has its own headroom requirement, and applying each channel's own preamp
    would be the tighter fit -- but it would also change the level of one
    speaker relative to the other, which moves the stereo image. A per-channel
    correction is supposed to fix each speaker's *response*, not re-balance the
    pair. So the applied preamp is the most negative any channel needs, which
    is safe for all of them and preserves their relative levels exactly.
    """

    curves: List[ChannelCurve] = field(default_factory=list)
    n_channels: int = 2

    def __len__(self) -> int:
        return len(self.curves)

    def __iter__(self):
        return iter(self.curves)

    @property
    def preamp_db(self) -> float:
        """The single preamp applied to every channel: the worst case."""
        if not self.curves:
            return 0.0
        return float(min(c.preamp_db for c in self.curves))

    @property
    def labels(self) -> List[str]:
        return [c.label for c in self.curves]

    def covers_all_channels(self) -> bool:
        """True when every channel of the layout has a curve."""
        return sorted(c.index for c in self.curves) == list(range(self.n_channels))


# ── Cross-channel referencing ─────────────────────────────────────────────────

def channel_levels_db(
    measurements: Sequence[Tuple[np.ndarray, np.ndarray]],
    reference_hz: float = 1000.0,
) -> List[float]:
    """Each channel's measured level at `reference_hz`, on the measurement's
    own uncalibrated scale. Only differences between them are meaningful."""
    return [
        float(np.interp(np.log10(reference_hz),
                        np.log10(np.maximum(np.asarray(f, dtype=float), 1e-6)),
                        np.asarray(m, dtype=float)))
        for f, m in measurements
    ]


def level_offsets_db(
    measurements: Sequence[Tuple[np.ndarray, np.ndarray]],
    reference_hz: float = 1000.0,
) -> List[float]:
    """
    Per-channel level trims that match the channels to each other.

    Returns one value per measurement, all **<= 0**: every channel is cut to
    the level of the quietest. Cutting rather than boosting means level
    matching can never introduce clipping, so it composes with the shared
    preamp without any headroom recalculation.

    These are gains, to be emitted as a `Preamp` line inside each channel's
    block -- not something the filter solver can or should express. See this
    module's docstring.

    Only sound when the measurements are comparable: same microphone, same
    gain, same position, one speaker at a time. A single mic that is closer to
    one speaker will report an imbalance that is geometry, not the speakers,
    and matching it would move the stereo image permanently.
    """
    if not measurements:
        return []
    levels = channel_levels_db(measurements, reference_hz)
    quietest = min(levels)
    return [quietest - lvl for lvl in levels]
