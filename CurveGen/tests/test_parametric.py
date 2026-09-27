"""
tests/test_parametric.py — Unit tests for the parametric (free Fc/Q/gain) solver.

Every synthetic room here is built out of peaking biquads via
`evaluate_eq_response_db`, so the correct answer is known in closed form: a
room that *is* a peaking filter is exactly cancelled by one filter at the same
Fc and Q with the opposite gain. That makes it possible to assert on the
recovered filter parameters, not just on the residual.
"""
import numpy as np
import pytest

from curvegen.flatten import DEFAULT_BAND_HZ, compute_correction
from curvegen.parametric import (
    DEFAULT_Q_RANGE,
    ParametricCurve,
    _merge_coincident,
    compute_parametric_correction,
    solve_parametric_filters,
)
from curvegen.response import evaluate_eq_response_db

SR = 48000.0


# ── Helpers ───────────────────────────────────────────────────────────────────

def room(defects, n=8192, sr=SR):
    """Synthetic room response: a cascade of peaking filters (fc, q, gain)."""
    freqs = np.fft.rfftfreq(n, d=1.0 / sr)
    mag = evaluate_eq_response_db(
        freqs,
        [fc for fc, _, _ in defects],
        [g for _, _, g in defects],
        q=[q for _, q, _ in defects],
        sample_rate=sr,
    )
    return freqs, mag


def grid(sr=SR, points=400):
    return np.logspace(np.log10(20.0), np.log10(min(20000.0, sr / 2 * 0.99)), points)


def residual(freqs, room_db, curve, sr=SR):
    """Corrected response on a dense grid, with the free constant removed.

    The constant is removed because it is level rather than shape -- the same
    reason the solver fits a centred residual (see parametric.py).
    """
    g = grid(sr)
    base = np.interp(np.log10(g), np.log10(np.maximum(freqs, 1e-6)), room_db)
    eq = evaluate_eq_response_db(
        g, list(curve.fc_hz), list(curve.gains_db), q=list(curve.q), sample_rate=sr)
    r = base + eq
    return g, r - np.mean(r)


def graphic_residual(freqs, room_db, sr=SR):
    """Same measurement corrected by the fixed ten-band solver, for comparison."""
    g = grid(sr)
    base = np.interp(np.log10(g), np.log10(np.maximum(freqs, 1e-6)), room_db)
    gains, _ = compute_correction(freqs, room_db, sample_rate=sr, auto_preamp=False)
    r = base + evaluate_eq_response_db(g, DEFAULT_BAND_HZ, gains, q=1.0, sample_rate=sr)
    return g, r - np.mean(r)


# ── The acceptance criterion from issue #4 ────────────────────────────────────

class TestNarrowResonance:
    """+10 dB at 90 Hz, Q 6 -- the case the fixed grid provably cannot fix.

    90 Hz sits between the 62 Hz and 125 Hz bands, and a Q=1 band is about an
    octave wide against a mode a sixth of an octave wide.
    """

    DEFECT = [(90.0, 6.0, 10.0)]

    def test_neighbouring_frequencies_are_not_disturbed(self):
        freqs, mag = room(self.DEFECT)
        curve = compute_parametric_correction(freqs, mag, sample_rate=SR)
        g, res = residual(freqs, mag, curve)
        away = (g < 45.0) | (g > 180.0)          # an octave clear either side
        assert np.max(np.abs(res[away])) < 1.0, (
            f"disturbed frequencies away from the mode by "
            f"{np.max(np.abs(res[away])):.2f} dB")

    def test_the_mode_itself_is_corrected(self):
        freqs, mag = room(self.DEFECT)
        curve = compute_parametric_correction(freqs, mag, sample_rate=SR)
        _, res = residual(freqs, mag, curve)
        assert np.max(np.abs(res)) < 1.0

    def test_fixed_grid_cannot_do_this(self):
        """Pins the limitation this module exists to lift.

        The ten-band solver is asserted to leave most of the defect in place --
        not merely to be worse. If this ever starts failing, the graphic solver
        has improved and the comparison above needs revisiting.
        """
        freqs, mag = room(self.DEFECT)
        _, res = graphic_residual(freqs, mag)
        assert np.max(np.abs(res)) > 4.0, (
            f"expected the fixed grid to leave a large residual; got "
            f"{np.max(np.abs(res)):.2f} dB")

    def test_it_places_one_filter_on_the_mode(self):
        """The solver should find the defect, not approximate it with a pile."""
        freqs, mag = room(self.DEFECT)
        curve = compute_parametric_correction(freqs, mag, sample_rate=SR)
        assert len(curve) <= 2, f"expected 1-2 filters, got {len(curve)}"
        i = int(np.argmax(np.abs(curve.gains_db)))
        assert 80.0 < curve.fc_hz[i] < 100.0
        assert curve.gains_db[i] < 0.0          # cutting a resonance
        assert curve.q[i] > 3.0                 # and doing it narrowly


# ── Recovering known rooms ────────────────────────────────────────────────────

class TestRecovery:

    def test_recovers_a_multi_defect_room(self):
        defects = [(58.0, 5.0, 9.0), (140.0, 2.0, -5.0),
                   (3200.0, 1.5, 4.0), (9000.0, 0.8, -6.0)]
        freqs, mag = room(defects)
        curve = compute_parametric_correction(freqs, mag, sample_rate=SR)
        _, res = residual(freqs, mag, curve)
        assert np.max(np.abs(res)) < 1.0

        # Each real defect should have a filter near it with the opposite sign.
        for fc, _, gain in defects:
            near = np.abs(np.log10(curve.fc_hz) - np.log10(fc)) < 0.05
            assert near.any(), f"no filter placed near {fc} Hz"
            assert np.sign(curve.gains_db[near][np.argmax(np.abs(curve.gains_db[near]))]) == -np.sign(gain)

    def test_beats_the_fixed_grid_on_the_phase_2_room(self):
        """The graphic solver's best case: defects on band centres at Q=1.

        Even here the parametric solver should win, because it can place
        exactly two filters instead of spreading ten.
        """
        freqs, mag = room([(125.0, 1.0, 8.0), (2000.0, 1.0, -6.0)])
        curve = compute_parametric_correction(freqs, mag, sample_rate=SR)
        _, res_p = residual(freqs, mag, curve)
        _, res_g = graphic_residual(freqs, mag)
        assert np.max(np.abs(res_p)) < np.max(np.abs(res_g))
        assert len(curve) <= 4

    def test_broad_tilt_beats_the_fixed_grid(self):
        """The solver's hardest case, asserted honestly.

        A -6 dB/decade tilt across the whole spectrum wants a shelf, and
        peaking filters bounded to Q >= 0.5 can only approximate one. The
        parametric solver does not get close to exact here -- it just beats
        the fixed grid comfortably.
        """
        freqs = np.fft.rfftfreq(8192, d=1.0 / SR)
        tilt = -6.0 * (np.log10(np.maximum(freqs, 20.0)) - np.log10(1000.0))
        curve = compute_parametric_correction(freqs, tilt, sample_rate=SR)
        _, res_p = residual(freqs, tilt, curve)
        _, res_g = graphic_residual(freqs, tilt)
        assert np.max(np.abs(res_p)) < np.max(np.abs(res_g)) / 2.0


# ── Solver mechanics and contracts ────────────────────────────────────────────

class TestSolverContracts:

    def test_respects_gain_q_and_frequency_bounds(self):
        freqs, mag = room([(90.0, 6.0, 20.0), (5000.0, 3.0, -18.0)])
        curve = compute_parametric_correction(
            freqs, mag, max_gain_db=6.0, q_range=(1.0, 4.0),
            freq_range=(50.0, 8000.0), sample_rate=SR)
        assert np.all(np.abs(curve.gains_db) <= 6.0 + 1e-6)
        assert np.all(curve.q >= 1.0 - 1e-6) and np.all(curve.q <= 4.0 + 1e-6)
        assert np.all(curve.fc_hz >= 50.0 - 1e-6) and np.all(curve.fc_hz <= 8000.0 + 1e-6)

    def test_filter_count_never_exceeds_the_budget(self):
        freqs, mag = room([(58.0, 5.0, 9.0), (140.0, 2.0, -5.0), (3200.0, 1.5, 4.0)])
        for budget in (1, 3, 10):
            curve = compute_parametric_correction(
                freqs, mag, n_filters=budget, sample_rate=SR)
            assert len(curve) <= budget

    def test_results_are_sorted_by_frequency(self):
        freqs, mag = room([(58.0, 5.0, 9.0), (3200.0, 1.5, 4.0), (140.0, 2.0, -5.0)])
        curve = compute_parametric_correction(freqs, mag, sample_rate=SR)
        assert np.all(np.diff(curve.fc_hz) > 0)

    def test_preamp_is_never_positive(self):
        freqs, mag = room([(90.0, 6.0, -10.0)])       # a dip, so the fix is a boost
        curve = compute_parametric_correction(freqs, mag, sample_rate=SR)
        assert curve.preamp_db <= 0.0

    def test_zero_filter_budget_returns_nothing(self):
        freqs, mag = room([(90.0, 6.0, 10.0)])
        with pytest.warns(UserWarning, match="placed no filters"):
            curve = compute_parametric_correction(freqs, mag, n_filters=0, sample_rate=SR)
        assert len(curve) == 0
        assert curve.preamp_db == 0.0

    def test_flat_room_gets_essentially_no_correction(self):
        freqs = np.fft.rfftfreq(4096, d=1.0 / SR)
        curve = compute_parametric_correction(freqs, np.zeros_like(freqs), sample_rate=SR)
        if len(curve):
            assert np.max(np.abs(curve.gains_db)) < 0.5

    def test_rejects_a_non_positive_q_range(self):
        g = np.logspace(np.log10(20), np.log10(20000), 64)
        with pytest.raises(ValueError, match="q_range"):
            solve_parametric_filters(g, np.zeros_like(g), q_range=(0.0, 4.0))

    def test_harman_target_tilts_the_delivered_curve(self):
        """Harman asks for more bass and less treble than a flat target.

        Asserted on the *delivered* response rather than on the gain values:
        the solver is free to satisfy the target with any filter layout, so
        which filters exist and what they are worth individually is not the
        contract -- the curve they add up to is.
        """
        freqs, mag = room([(125.0, 1.0, 8.0)])

        def delivered(curve, hz):
            at = evaluate_eq_response_db(
                [hz, 1000.0], list(curve.fc_hz), list(curve.gains_db),
                q=list(curve.q), sample_rate=SR)
            return at[0] - at[1]        # relative to the 1 kHz reference

        flat = compute_parametric_correction(freqs, mag, sample_rate=SR)
        harman = compute_parametric_correction(
            freqs, mag, use_harman_target=True, sample_rate=SR)

        assert delivered(harman, 80.0) > delivered(flat, 80.0) + 0.5
        assert delivered(harman, 8000.0) < delivered(flat, 8000.0) - 0.5


class TestMergeCoincident:
    """The step that stops the refinement parking two filters on one feature."""

    def test_merges_filters_at_the_same_frequency(self):
        fc = np.array([100.0, 100.5, 4000.0])
        q = np.array([1.0, 1.0, 2.0])
        g = np.array([-4.0, -2.0, 3.0])
        mfc, mq, mg = _merge_coincident(fc, q, g)
        assert len(mfc) == 2
        # Cascaded peaking filters add in dB, so co-located gains sum.
        assert mg[0] == pytest.approx(-6.0)
        assert 100.0 <= mfc[0] <= 100.5
        assert mg[1] == pytest.approx(3.0)

    def test_leaves_well_separated_filters_alone(self):
        fc = np.array([100.0, 1000.0, 10000.0])
        q = np.array([1.0, 1.0, 1.0])
        g = np.array([1.0, -2.0, 3.0])
        mfc, _, mg = _merge_coincident(fc, q, g)
        assert len(mfc) == 3
        np.testing.assert_allclose(mg, g)

    def test_single_filter_is_returned_unchanged(self):
        fc, q, g = np.array([100.0]), np.array([1.0]), np.array([-3.0])
        mfc, mq, mg = _merge_coincident(fc, q, g)
        np.testing.assert_allclose(mfc, fc)
        np.testing.assert_allclose(mg, g)


class TestParametricCurveType:

    def test_is_a_curve_with_parallel_arrays(self):
        freqs, mag = room([(90.0, 6.0, 10.0)])
        curve = compute_parametric_correction(freqs, mag, sample_rate=SR)
        assert isinstance(curve, ParametricCurve)
        assert len(curve.fc_hz) == len(curve.q) == len(curve.gains_db) == len(curve)

    def test_default_q_range_is_wider_than_the_graphic_solver(self):
        """The whole point: Q is chosen, not fixed at 1.0."""
        assert DEFAULT_Q_RANGE[0] < 1.0 < DEFAULT_Q_RANGE[1]
