"""
parametric.py — Fit a correction curve using filters whose centre frequency,
                Q and gain are all chosen by the solver.

Relationship to `flatten.py`
----------------------------
`flatten.py` solves a **graphic** EQ: a fixed grid of centre frequencies at one
shared Q, where the only free parameters are the gains. That matches
`DSP::Equalizer10Band` exactly (ten bands, shared Q) and stays the default.

This module solves a **parametric** EQ: N filters, each with its own
(Fc, Q, gain). Rooms do not put their defects on ISO band centres, and the two
common defect shapes pull in opposite directions:

  - A **narrow room mode** -- the dominant low-frequency problem -- needs a
    high-Q filter placed on it. A Q=1 band an octave wide gouges a broad hole
    around the mode while barely denting it. On a synthetic +10 dB Q=6
    resonance at 90 Hz, the fixed ten-band grid leaves 6.7 dB of error; one
    freely-placed filter leaves 0.6 dB.
  - A **broad tilt** is the opposite: fixed Q=1 bands spend several filters on
    it and leave ripple behind (5.1 dB worst-case error on a synthetic tilt,
    against 0.03 dB here).

Where the output can actually go
--------------------------------
Only `eqapo_export` can express this fully. Checked, not assumed:

  - `eqapo_export.render_eqapo_config` already takes per-filter Q and any
    number of filters. It is the intended sink.
  - `response.evaluate_eq_response_db` already models per-band Q, so the
    curve can be evaluated and plotted.
  - `shared/preset_schema.json` pins `bands` to exactly 10 entries
    (`minItems`/`maxItems`), so a JSON preset cannot carry an arbitrary filter
    count -- even though the schema does allow a per-band `q`.
  - `DSP::Equalizer10Band` is `BandCount = 10` with a *single shared* Q
    (`SetBandsPeaking(centres, gains, Q)`), so this project's own DSP cannot
    apply a per-filter-Q curve at all, whatever the file format says.
  - The IPC `set_bands` command carries gains only -- no centres, no Q.

So a parametric curve is realisable today through Equalizer APO, which is
also the only playback path this project has actually validated on hardware
(`TEST_RESULTS.md`). Widening `DSP::Equalizer10Band`, the preset schema and
the IPC protocol is separate work and is deliberately not attempted here.

The algorithm
-------------
1. **Greedy seeding.** Repeatedly find the largest remaining deviation, place
   a filter there, and fit that one filter's (Fc, Q, gain) to the residual.
   Q is seeded from the deviation's own half-height bandwidth rather than a
   constant, which is what lets a narrow mode be matched by a narrow filter
   instead of being approached by a sequence of wide ones.
2. **Stop early.** Adding a filter that does not reduce residual RMS by at
   least `min_improvement` dB is not worth it. Without this the solver spends
   its whole budget every time and produces degenerate stacks of near-identical
   filters that partly cancel -- measured: 10 filters with 36 coincident pairs
   on a problem one filter solves, and 29 s to do it.
3. **Joint refinement.** Re-fit every (Fc, Q, gain) simultaneously against the
   true cascade, seeded from the greedy result.
4. **Merge** filters that ended up on top of one another, and refine again
   from that reduced seed.
5. **Prune** filters whose final gain is negligible.

Two details that matter more than they look:

**Fc is optimised in log10.** Centre frequencies span three decades while Q
spans about one and gain spans ±12 dB. Optimising Fc directly leaves the
problem badly scaled and `least_squares` crawls; in log10 all three parameters
have comparable range.

**The residual is centred (`_centred`).** A constant dB offset across the whole
spectrum is not something a peaking filter can produce -- and not something it
should have to, because a constant is just volume, absorbed by the preamp or
the user's volume knob. Fitting `target - cascade` without removing its mean
makes the solver burn filters chasing that constant. Removing it analytically
(the optimal constant *is* the residual's mean, so this is an exact projection,
not an approximation) is equivalent to treating overall level as a free
parameter. It is worth a large amount: on the synthetic Phase 2 room this alone
took the worst-case error from 0.85 dB to 0.03 dB.

Note a subtlety that pushed an earlier draft the wrong way: an L2 penalty on
gains, added to discourage redundant filters, does the opposite. Splitting one
-4.9 dB filter into three -1.6 dB ones *lowers* the sum of squares
(3 x 1.64^2 = 8.1 versus 4.9^2 = 24), so the penalty actively rewards
duplication. Filter count is controlled by the early stop in step 2 instead.
"""
from __future__ import annotations

import warnings
from dataclasses import dataclass
from typing import Optional, Sequence, Tuple

import numpy as np
from scipy.optimize import least_squares

from .flatten import MAX_GAIN_DB, MIN_FREQ_HZ, _target_curve
from .response import DEFAULT_SAMPLE_RATE, cascade_peak_db, evaluate_eq_response_db

# ── Defaults ──────────────────────────────────────────────────────────────────

DEFAULT_FILTERS: int = 10
DEFAULT_Q_RANGE: Tuple[float, float] = (0.5, 12.0)
DEFAULT_FREQ_RANGE: Tuple[float, float] = (20.0, 20000.0)

# Log-spaced points the fit is evaluated on. Denser grids did not improve any
# test case; sparser ones under-resolve high-Q filters.
GRID_POINTS: int = 256

# Minimum reduction in residual RMS (dB) a new filter must deliver to be worth
# adding. 0.02 was chosen by sweeping {0.1, 0.05, 0.02, 0.01} across four
# synthetic rooms: it is the largest value that is not beaten by a smaller one
# on any of them, and it keeps the solve under ~2.5 s in the worst case.
MIN_IMPROVEMENT_DB: float = 0.02

# Filters weaker than this are dropped from the result rather than emitted as
# near-inaudible clutter in the config file.
PRUNE_BELOW_DB: float = 0.1

# Two filters closer together than this (in decades of centre frequency) are
# treated as one and merged before a final refinement pass. ~0.03 decades is
# about 7%, comfortably inside the width of even a Q=12 filter, so filters this
# close are describing the same feature rather than two different ones.
MERGE_DECADES: float = 0.03

# How many merge/refine rounds to attempt. Refinement can re-create a
# coincident pair that the previous merge just removed, so one pass is not
# enough; three is comfortably past the point where any test case still changes.
MAX_MERGE_PASSES: int = 3

# Cap on `least_squares` function evaluations, per free parameter.
#
# This bounds the worst case rather than improving the typical one. When the
# target demands more correction than `max_gain_db` permits, every filter
# saturates at its bound and the solve never satisfies xtol/ftol -- measured:
# a 30-parameter fit ran to scipy's own default cap (100 x parameters = 3000
# evaluations) and took 44 s to return a result no better than the one it had
# after a few dozen. Healthy fits converge in 25-87 evaluations regardless of
# size, so 10 per parameter leaves them untouched with 3x headroom while
# cutting that pathological case to a few seconds.
MAX_NFEV_PER_PARAM: int = 10


def _nfev_cap(n_params: int) -> int:
    return max(50, MAX_NFEV_PER_PARAM * n_params)


@dataclass
class ParametricCurve:
    """A solved parametric correction curve.

    `fc_hz`, `q` and `gains_db` are parallel arrays, ordered by ascending
    centre frequency. `len(fc_hz)` is at most the requested filter count and
    is often less -- see the module docstring on early stopping.
    """

    fc_hz: np.ndarray
    q: np.ndarray
    gains_db: np.ndarray
    preamp_db: float
    #: Constant dB offset between the delivered curve and the requested target.
    #: Informational: it is level, not shape, and is absorbed by the preamp or
    #: the volume control. It is deliberately *not* folded into `preamp_db`,
    #: which exists to prevent clipping and must stay <= 0.
    offset_db: float

    def __len__(self) -> int:
        return len(self.fc_hz)


# ── Internals ─────────────────────────────────────────────────────────────────

def _centred(x: np.ndarray) -> np.ndarray:
    """Remove the free constant (see module docstring)."""
    return x - np.mean(x)


def _rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(x))))


def _one(grid: np.ndarray, fc: float, q: float, gain: float, sr: float) -> np.ndarray:
    return evaluate_eq_response_db(grid, [fc], [gain], q=q, sample_rate=sr)


def _cascade(grid: np.ndarray, fc, q, gains, sr: float) -> np.ndarray:
    return evaluate_eq_response_db(grid, list(fc), list(gains), q=list(q), sample_rate=sr)


def _seed_q(grid: np.ndarray, resid: np.ndarray, i: int, q_lo: float, q_hi: float) -> float:
    """Estimate a filter's Q from the width of the deviation it will correct.

    Walks outward from the peak at `i` until the deviation falls below half its
    height or changes sign, and reads Q as `fc / bandwidth` -- the textbook
    definition. Seeding a constant Q instead is what made an earlier draft
    approach a narrow mode with a stack of progressively narrower filters
    rather than placing one correct filter immediately.
    """
    peak = resid[i]
    half = abs(peak) / 2.0
    lo = hi = i
    while lo > 0 and abs(resid[lo]) > half and np.sign(resid[lo]) == np.sign(peak):
        lo -= 1
    while hi < len(resid) - 1 and abs(resid[hi]) > half and np.sign(resid[hi]) == np.sign(peak):
        hi += 1
    if grid[hi] <= grid[lo]:
        return float(np.clip(2.0, q_lo, q_hi))
    return float(np.clip(grid[i] / (grid[hi] - grid[lo]), q_lo, q_hi))


def _merge_coincident(fc, q, gains, tol_decades: float = MERGE_DECADES):
    """Collapse filters sitting on top of each other into one.

    The joint refinement is free to park two filters at nearly the same centre
    frequency with opposing gains that largely cancel. The net response is
    fine, but the representation is not: it wastes filters, it reads as
    nonsense in a config file, and -- because auto-preamp is driven by the
    cascade's true peak -- a +12/-12 dB pair costs real headroom for no
    audible benefit. One observed case pushed the preamp to -9.6 dB.

    Cascaded peaking filters add in dB, so co-located filters of equal Q merge
    exactly by summing their gains. Q differs in practice, so the merged Q is
    the |gain|-weighted mean and the result is treated as a *seed* for one more
    refinement rather than as a final answer.
    """
    if len(fc) < 2:
        return fc, q, gains

    order = np.argsort(fc)
    fc, q, gains = fc[order], q[order], gains[order]

    groups: list[list[int]] = [[0]]
    for i in range(1, len(fc)):
        if abs(np.log10(fc[i]) - np.log10(fc[groups[-1][-1]])) <= tol_decades:
            groups[-1].append(i)
        else:
            groups.append([i])

    out_fc, out_q, out_g = [], [], []
    for grp in groups:
        if len(grp) == 1:
            j = grp[0]
            out_fc.append(fc[j]); out_q.append(q[j]); out_g.append(gains[j])
            continue
        w = np.abs(gains[grp])
        if w.sum() <= 0:
            continue
        out_fc.append(float(np.exp(np.average(np.log(fc[grp]), weights=w))))
        out_q.append(float(np.average(q[grp], weights=w)))
        out_g.append(float(np.sum(gains[grp])))

    return np.asarray(out_fc), np.asarray(out_q), np.asarray(out_g)


# ── Public API ────────────────────────────────────────────────────────────────

def solve_parametric_filters(
    target_hz: np.ndarray,
    target_db: np.ndarray,
    n_filters: int = DEFAULT_FILTERS,
    q_range: Tuple[float, float] = DEFAULT_Q_RANGE,
    freq_range: Tuple[float, float] = DEFAULT_FREQ_RANGE,
    max_gain_db: float = MAX_GAIN_DB,
    sample_rate: float = DEFAULT_SAMPLE_RATE,
    grid_points: int = GRID_POINTS,
    min_improvement_db: float = MIN_IMPROVEMENT_DB,
    prune_below_db: float = PRUNE_BELOW_DB,
) -> Tuple[np.ndarray, np.ndarray, np.ndarray, float]:
    """
    Fit up to `n_filters` peaking filters to `target_db`, choosing each
    filter's centre frequency, Q and gain.

    `target_db` is the correction curve wanted *from the EQ* (not the measured
    room response) sampled at `target_hz` -- the same convention as
    `flatten._target_curve`.

    Returns `(fc_hz, q, gains_db, offset_db)`, the first three ordered by
    ascending centre frequency, and the last the constant dB offset between
    the delivered curve and the target (level, not shape -- see
    `ParametricCurve.offset_db`).
    """
    n_filters = max(int(n_filters), 0)
    if n_filters == 0:
        return np.array([]), np.array([]), np.array([]), 0.0

    q_lo, q_hi = (float(min(q_range)), float(max(q_range)))
    if q_lo <= 0:
        raise ValueError(f"q_range must be strictly positive, got {q_range!r}")
    max_gain_db = float(abs(max_gain_db))
    sr = sample_rate if sample_rate and sample_rate > 0 else DEFAULT_SAMPLE_RATE

    # Fit only where the measurement has data, and never above Nyquist. np.interp
    # clamps rather than extrapolating, so a grid running past the measured range
    # would let the solver place filters to "correct" a flat extrapolation.
    target_hz = np.asarray(target_hz, dtype=float)
    target_db = np.asarray(target_db, dtype=float)
    f_lo = max(float(min(freq_range)), MIN_FREQ_HZ, float(np.min(target_hz)))
    f_hi = min(float(max(freq_range)), 20000.0, sr / 2.0 * 0.99, float(np.max(target_hz)))
    if not (f_hi > f_lo):
        return np.array([]), np.array([]), np.array([]), 0.0

    grid = np.logspace(np.log10(f_lo), np.log10(f_hi), max(int(grid_points), 32))
    target = np.interp(np.log10(grid), np.log10(target_hz), target_db)

    log_lo, log_hi = np.log10(f_lo), np.log10(f_hi)
    lower = [log_lo, q_lo, -max_gain_db]
    upper = [log_hi, q_hi, max_gain_db]

    # ── 1-2. greedy seeding with an early stop ───────────────────────────────
    found: list[np.ndarray] = []
    resid = _centred(target)
    prev_rms = _rms(resid)

    for _ in range(n_filters):
        i = int(np.argmax(np.abs(resid)))
        if abs(resid[i]) < 1e-3:
            break
        seed = [
            float(np.clip(np.log10(grid[i]), log_lo, log_hi)),
            _seed_q(grid, resid, i, q_lo, q_hi),
            float(np.clip(resid[i], -max_gain_db, max_gain_db)),
        ]
        sol = least_squares(
            lambda p, r=resid: _centred(r - _one(grid, 10.0 ** p[0], p[1], p[2], sr)),
            seed, bounds=(lower, upper), method="trf", xtol=1e-10, ftol=1e-10,
            max_nfev=_nfev_cap(3),
        )
        stepped = _centred(resid - _one(grid, 10.0 ** sol.x[0], sol.x[1], sol.x[2], sr))
        if found and (prev_rms - _rms(stepped)) < min_improvement_db:
            break
        found.append(sol.x)
        resid = stepped
        prev_rms = _rms(resid)

    if not found:
        return np.array([]), np.array([]), np.array([]), 0.0

    # ── 3. joint refinement of every (Fc, Q, gain) at once ───────────────────
    seeds = np.asarray(found)
    n = len(seeds)
    p0 = np.concatenate([seeds[:, 0], seeds[:, 1], seeds[:, 2]])
    lo = np.concatenate([[log_lo] * n, [q_lo] * n, [-max_gain_db] * n])
    hi = np.concatenate([[log_hi] * n, [q_hi] * n, [max_gain_db] * n])

    sol = least_squares(
        lambda p: _centred(target - _cascade(grid, 10.0 ** p[:n], p[n:2 * n], p[2 * n:], sr)),
        p0, bounds=(lo, hi), method="trf", xtol=1e-10, ftol=1e-10,
        max_nfev=_nfev_cap(3 * n),
    )
    fc = 10.0 ** sol.x[:n]
    q = sol.x[n:2 * n]
    gains = sol.x[2 * n:]

    # ── 4. merge co-located filters and re-refine, repeatedly ────────────────
    #
    # This has to loop. Refining after a merge is free to park two filters on
    # top of each other again -- observed directly: a single merge pass cleaned
    # up one cancelling pair and the following refinement created a fresh
    # -6.4/+12.0 dB pair 175 Hz apart. Iterate until a pass changes nothing.
    for _ in range(MAX_MERGE_PASSES):
        merged_fc, merged_q, merged_gains = _merge_coincident(fc, q, gains)
        m = len(merged_fc)
        if m == 0 or m >= len(fc):
            break

        p0 = np.concatenate([
            np.clip(np.log10(merged_fc), log_lo, log_hi),
            np.clip(merged_q, q_lo, q_hi),
            np.clip(merged_gains, -max_gain_db, max_gain_db),
        ])
        lo = np.concatenate([[log_lo] * m, [q_lo] * m, [-max_gain_db] * m])
        hi = np.concatenate([[log_hi] * m, [q_hi] * m, [max_gain_db] * m])
        sol2 = least_squares(
            lambda p: _centred(target - _cascade(grid, 10.0 ** p[:m], p[m:2 * m], p[2 * m:], sr)),
            p0, bounds=(lo, hi), method="trf", xtol=1e-10, ftol=1e-10,
            max_nfev=_nfev_cap(3 * m),
        )

        # Accept the merge on the same terms the forward pass adds filters on:
        # a filter has to buy `min_improvement_db` of residual RMS to be worth
        # keeping, so giving up (k) filters may cost that much per filter.
        #
        # Judging it any more strictly does not work. The pre-merge solution has
        # strictly more free parameters, so in a least-squares sense it almost
        # always fits better; requiring the merged fit to match it rejected
        # every merge that mattered, including ones that removed a cancelling
        # +12/-12 dB pair.
        dropped = len(fc) - m
        before = _rms(_centred(target - _cascade(grid, fc, q, gains, sr)))
        cand_fc = 10.0 ** sol2.x[:m]
        cand_q = sol2.x[m:2 * m]
        cand_gains = sol2.x[2 * m:]
        after = _rms(_centred(target - _cascade(grid, cand_fc, cand_q, cand_gains, sr)))
        if after > before + min_improvement_db * dropped:
            break
        fc, q, gains = cand_fc, cand_q, cand_gains

    # ── 5. prune ─────────────────────────────────────────────────────────────
    keep = np.abs(gains) >= prune_below_db
    if not keep.any():                       # everything is tiny; keep the largest
        keep = np.abs(gains) >= np.max(np.abs(gains))
    fc, q, gains = fc[keep], q[keep], gains[keep]

    order = np.argsort(fc)
    fc, q, gains = fc[order], q[order], gains[order]

    offset = float(np.mean(target - _cascade(grid, fc, q, gains, sr)))
    return fc, q, gains, offset


def compute_parametric_correction(
    freqs: np.ndarray,
    magnitude_db: np.ndarray,
    n_filters: int = DEFAULT_FILTERS,
    q_range: Tuple[float, float] = DEFAULT_Q_RANGE,
    freq_range: Tuple[float, float] = DEFAULT_FREQ_RANGE,
    max_gain_db: float = MAX_GAIN_DB,
    use_harman_target: bool = False,
    harman_blend: float = 0.5,
    auto_preamp: bool = True,
    sample_rate: float = DEFAULT_SAMPLE_RATE,
) -> ParametricCurve:
    """
    Parametric counterpart to `flatten.compute_correction`.

    Takes the same measured response and target options, and returns filters
    whose centre frequency and Q were chosen by the solver rather than assumed.
    There is no `band_hz` or `q` parameter, because deciding those is the whole
    point; `freq_range` and `q_range` bound the search instead.
    """
    sr = float(sample_rate) if sample_rate else DEFAULT_SAMPLE_RATE

    usable = np.asarray(freqs, dtype=float) >= MIN_FREQ_HZ
    target_hz = np.asarray(freqs, dtype=float)[usable] if np.any(usable) else np.asarray(freqs, dtype=float)
    if target_hz.size == 0:
        raise ValueError("no usable measurement points above MIN_FREQ_HZ")

    target_db = _target_curve(target_hz, freqs, magnitude_db, use_harman_target, harman_blend)

    fc, q, gains, offset = solve_parametric_filters(
        target_hz, target_db,
        n_filters=n_filters, q_range=q_range, freq_range=freq_range,
        max_gain_db=max_gain_db, sample_rate=sr,
    )

    if len(fc) == 0:
        warnings.warn(
            "The parametric solver placed no filters -- the measured response is "
            "already flat within the solver's threshold, or the frequency range "
            "excludes all of the measurement.",
            UserWarning, stacklevel=2,
        )

    preamp_db = 0.0
    if auto_preamp and len(fc):
        peak_db = cascade_peak_db(fc, gains, q=list(q), sample_rate=sr)
        if peak_db > 0:
            preamp_db = -peak_db

    return ParametricCurve(
        fc_hz=fc, q=q, gains_db=gains, preamp_db=preamp_db, offset_db=offset,
    )
