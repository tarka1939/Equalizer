"""
tests/test_flatten.py — Unit tests for the flatten.py correction algorithm.
"""
import numpy as np
import pytest
from curvegen.flatten import (
    compute_correction,
    apply_octave_normalisation,
    solve_cascade_gains,
    DEFAULT_BAND_HZ,
)
from curvegen.response import evaluate_eq_response_db


def make_flat_response(n: int = 2048, sr: float = 48000.0):
    """Return a perfectly flat (0 dB) frequency response."""
    freqs = np.fft.rfftfreq(n, d=1.0 / sr)
    return freqs, np.zeros_like(freqs)


def make_boosted_response(boost_hz: float, boost_db: float, n: int = 2048, sr: float = 48000.0):
    """Return a response with a Gaussian peak at boost_hz."""
    freqs = np.fft.rfftfreq(n, d=1.0 / sr)
    mag   = boost_db * np.exp(-0.5 * ((np.log10(np.maximum(freqs, 1)) - np.log10(boost_hz)) / 0.3) ** 2)
    return freqs, mag


# ── Tests ─────────────────────────────────────────────────────────────────────

class TestComputeCorrection:

    def test_flat_input_gives_zero_correction(self):
        freqs, mag = make_flat_response()
        gains, preamp = compute_correction(freqs, mag)
        assert gains.shape == (10,)
        np.testing.assert_allclose(gains, 0.0, atol=0.5)

    def test_correction_inverts_boost(self):
        """A +6 dB peak at 4 kHz (relative to the 1 kHz reference) should
        produce a negative correction there.

        Note: a boost located exactly AT 1 kHz is a degenerate case for this
        algorithm, since every band's gain is computed relative to whatever
        the response measures at 1 kHz (see compute_correction's docstring/
        comments) — a peak at the reference frequency itself always
        corrects to ~0 by construction. So this test uses a different band
        to exercise the actual inversion behaviour.
        """
        freqs, mag = make_boosted_response(4000, 6.0)
        gains, _ = compute_correction(freqs, mag, auto_preamp=False)
        # Band index 7 is 4000 Hz
        assert gains[7] < -3.0, f"Expected negative correction at 4kHz, got {gains[7]:.2f}"

    def test_boost_at_reference_frequency_is_self_cancelling(self):
        """A boost located exactly at 1 kHz (the reference point) cannot be
        corrected, since every gain is defined relative to the 1 kHz level.

        The invariant is about the **cascade's response** at 1 kHz, not about
        the 1 kHz band's own gain value. Those are different numbers once the
        bands overlap, and asserting the latter is how this test used to pass
        while the property it names was false: under the old pointwise solver
        `gains[5]` was exactly 0.00, but the neighbouring bands' boosts leaked
        in and the chain actually applied **+2.39 dB** at 1 kHz. The cascade
        solver sets `gains[5]` to about -1.6 dB precisely so the delivered
        response there stays at unity.

        See ARCHITECTURE.md 6/7.4 for why the limitation exists at all.
        """
        freqs, mag = make_boosted_response(1000, 6.0)
        gains, _ = compute_correction(freqs, mag, auto_preamp=False)
        delivered = evaluate_eq_response_db(
            [1000.0], DEFAULT_BAND_HZ, gains, q=1.0, sample_rate=48000.0)[0]
        assert abs(delivered) < 0.5, (
            f"Expected ~0 dB delivered at the 1 kHz reference, got {delivered:+.2f}")

    def test_pointwise_solver_violates_the_reference_invariant(self):
        """Pins the defect the cascade solver exists to fix (issue #3).

        Same input as the test above. The pointwise solver reports a 1 kHz
        band gain of exactly zero while delivering a large boost there,
        because it never asks what the *cascade* does. If this ever starts
        passing, the pointwise path has been changed and the comparison in
        the test above is no longer meaningful.
        """
        freqs, mag = make_boosted_response(1000, 6.0)
        gains, _ = compute_correction(freqs, mag, auto_preamp=False, solver="pointwise")
        delivered = evaluate_eq_response_db(
            [1000.0], DEFAULT_BAND_HZ, gains, q=1.0, sample_rate=48000.0)[0]
        assert abs(gains[5]) < 1e-9, "pointwise should still report exactly 0 at the reference band"
        assert delivered > 1.5, (
            "pointwise is expected to deliver a large boost at 1 kHz despite asking "
            f"for 0 dB there; got {delivered:+.2f}")

    def test_gains_clipped_to_max(self):
        """Gains must never exceed max_gain_db."""
        freqs, mag = make_boosted_response(500, 30.0)
        gains, _ = compute_correction(freqs, mag, max_gain_db=12.0)
        assert np.all(np.abs(gains) <= 12.0 + 1e-6)

    def test_auto_preamp_is_non_positive(self):
        freqs, mag = make_boosted_response(125, -8.0)
        _, preamp = compute_correction(freqs, mag, auto_preamp=True)
        assert preamp <= 0.0

    def test_harman_enabled(self):
        freqs, mag = make_flat_response()
        gains_flat, _   = compute_correction(freqs, mag, use_harman_target=False)
        gains_harman, _ = compute_correction(freqs, mag, use_harman_target=True)
        # With Harman target on a flat input the low frequencies should be boosted.
        assert gains_harman[0] > gains_flat[0]

    def test_output_length_matches_bands(self):
        freqs, mag = make_flat_response()
        for n_bands in [5, 10, 15]:
            band_hz = np.logspace(np.log10(20), np.log10(20000), n_bands)
            gains, _ = compute_correction(freqs, mag, band_hz=band_hz)
            assert len(gains) == n_bands


class TestCascadeSolver:
    """Issue #3: gains must be solved against the cascade, not band by band.

    Every room here is built from peaking biquads via
    `evaluate_eq_response_db`, so the correct answer is known in closed form
    rather than approximated: a room that *is* a peaking filter at a band
    centre is exactly cancellable by that band at the opposite gain.
    """

    SR = 48000.0

    def room(self, defects, n=4096):
        """Synthetic room response: a cascade of peaking filters."""
        freqs = np.fft.rfftfreq(n, d=1.0 / self.SR)
        mag = evaluate_eq_response_db(
            freqs, [f for f, _ in defects], [g for _, g in defects],
            q=1.0, sample_rate=self.SR)
        return freqs, mag

    def residual_db(self, freqs, room_db, gains, lo=30.0, hi=16000.0):
        """Corrected response, referenced to 1 kHz, on a dense log grid."""
        grid = np.logspace(np.log10(lo), np.log10(hi), 400)
        room = np.interp(np.log10(grid), np.log10(np.maximum(freqs, 1e-6)), room_db)
        eq = evaluate_eq_response_db(grid, DEFAULT_BAND_HZ, gains, q=1.0, sample_rate=self.SR)
        res = room + eq
        return res - np.interp(np.log10(1000.0), np.log10(grid), res)

    # ── Acceptance criteria from the issue ────────────────────────────────────

    def test_closed_form_room_is_corrected_to_within_half_a_db(self):
        """`TEST_PLAN.md` Phase 2's room: +8 dB at 125 Hz, -6 dB at 2 kHz.

        Both defects sit on band centres at the band Q, so a perfect solver
        would return exactly [0,0,-8,0,0,0,+6,0,0,0] and leave zero residual.
        """
        freqs, room = self.room([(125.0, 8.0), (2000.0, -6.0)])
        gains, _ = compute_correction(freqs, room, auto_preamp=False)
        res = self.residual_db(freqs, room, gains)
        assert np.max(np.abs(res)) < 0.5, (
            f"worst residual {np.max(np.abs(res)):.2f} dB exceeds the 0.5 dB "
            "acceptance bar for a closed-form room")

    def test_cascade_solver_beats_pointwise_on_the_same_room(self):
        """Pins the size of the improvement, not just its direction."""
        freqs, room = self.room([(125.0, 8.0), (2000.0, -6.0)])
        casc, _ = compute_correction(freqs, room, auto_preamp=False)
        point, _ = compute_correction(freqs, room, auto_preamp=False, solver="pointwise")
        r_casc = np.sqrt(np.mean(self.residual_db(freqs, room, casc) ** 2))
        r_point = np.sqrt(np.mean(self.residual_db(freqs, room, point) ** 2))
        assert r_casc < r_point / 4.0, (
            f"cascade RMS {r_casc:.3f} dB should be far below pointwise {r_point:.3f} dB")

    def test_flat_band_is_not_damaged_by_a_large_neighbouring_correction(self):
        """The observed field failure: correcting 62 Hz wrecked 125 Hz.

        The room is flat at 125 Hz and has a big resonance one octave below.
        Correcting it must not push 125 Hz off flat.
        """
        freqs, room = self.room([(62.0, 9.0)])
        gains, _ = compute_correction(freqs, room, auto_preamp=False)
        res = self.residual_db(freqs, room, gains)
        grid = np.logspace(np.log10(30.0), np.log10(16000.0), 400)
        at_125 = float(np.interp(np.log10(125.0), np.log10(grid), res))
        assert abs(at_125) < 0.75, (
            f"125 Hz was flat before correction and is {at_125:+.2f} dB after it")

    def test_pointwise_does_damage_that_flat_band(self):
        """The same room through the old solver, so the regression is pinned."""
        freqs, room = self.room([(62.0, 9.0)])
        gains, _ = compute_correction(freqs, room, auto_preamp=False, solver="pointwise")
        grid = np.logspace(np.log10(30.0), np.log10(16000.0), 400)
        at_125 = float(np.interp(np.log10(125.0),
                                 np.log10(grid), self.residual_db(freqs, room, gains)))
        assert at_125 < -1.0, (
            f"pointwise is expected to drag the flat 125 Hz band down; got {at_125:+.2f}")

    # ── Solver mechanics ──────────────────────────────────────────────────────

    def test_gains_respect_bounds(self):
        freqs, room = self.room([(125.0, 40.0), (250.0, -40.0)])
        gains, _ = compute_correction(freqs, room, max_gain_db=6.0, auto_preamp=False)
        assert np.all(np.abs(gains) <= 6.0 + 1e-6)

    def test_widely_spaced_bands_agree_with_pointwise(self):
        """Sanity check on the solver itself.

        With bands three octaves apart their skirts barely overlap, so the
        cascade assumption collapses to the pointwise one and the two solvers
        must agree. If they diverge here, the solver is wrong rather than
        merely different.

        The defect is placed at 8 kHz, far from the 1 kHz reference, so that
        the room measures ~0 dB at 1 kHz and the target curve is ~0 away from
        the defect. That matters: put the defect near 1 kHz instead and the
        reference lands on its skirt, which turns the target into a broadband
        offset that three narrow filters genuinely cannot represent -- the
        solvers then disagree for a reason that has nothing to do with
        overlap. See `test_sparse_bands_cannot_represent_a_broadband_target`.
        """
        bands = [125.0, 1000.0, 8000.0]
        freqs, room = self.room([(8000.0, 6.0)])
        casc, _ = compute_correction(freqs, room, band_hz=bands, auto_preamp=False)
        point, _ = compute_correction(freqs, room, band_hz=bands, auto_preamp=False,
                                      solver="pointwise")
        np.testing.assert_allclose(casc, point, atol=0.5)

    def test_sparse_bands_cannot_represent_a_broadband_target(self):
        """Documents a real limitation of fitting over a grid, not a bug.

        Three Q=1 filters cannot synthesise a near-constant offset across the
        whole spectrum. Asked to, least-squares spreads the error: it drives
        each band harder than its centre needs so the skirts cover more of the
        gap. The delivered response at a band centre then overshoots what that
        band nominally asked for.

        This is why band gains stop being individually interpretable once the
        solver is cascade-aware, and it is an argument for issue #4 (free
        centre frequencies and per-band Q) rather than something to "fix" in
        the solver. It does not affect the default ten-band set, which is
        dense enough to track a broadband target -- covered by
        `test_broadband_tilt_is_corrected_better_than_pointwise`.
        """
        bands = [100.0, 800.0, 6400.0]
        freqs, room = self.room([(800.0, 6.0)])
        casc, _ = compute_correction(freqs, room, band_hz=bands, auto_preamp=False)
        point, _ = compute_correction(freqs, room, band_hz=bands, auto_preamp=False,
                                      solver="pointwise")
        assert np.max(np.abs(casc - point)) > 1.0, (
            "expected the sparse-band solvers to diverge on a broadband target")

    def test_broadband_tilt_is_corrected_better_than_pointwise(self):
        """A tilted room, which is the case most likely to make a grid fit
        over-boost. It does not: the cascade solver lands closer *and* uses a
        smaller maximum gain than pointwise inversion.
        """
        freqs = np.fft.rfftfreq(4096, d=1.0 / self.SR)
        tilt = -6.0 * (np.log10(np.maximum(freqs, 20.0)) - np.log10(1000.0))
        casc, _ = compute_correction(freqs, tilt, auto_preamp=False)
        point, _ = compute_correction(freqs, tilt, auto_preamp=False, solver="pointwise")
        r_casc = np.sqrt(np.mean(self.residual_db(freqs, tilt, casc) ** 2))
        r_point = np.sqrt(np.mean(self.residual_db(freqs, tilt, point) ** 2))
        assert r_casc < r_point
        assert np.max(np.abs(casc)) <= np.max(np.abs(point)) + 1e-9

    def test_flat_room_needs_no_correction(self):
        freqs, room = self.room([(1000.0, 0.0)])
        gains, _ = compute_correction(freqs, room, auto_preamp=False)
        assert np.max(np.abs(gains)) < 0.2

    def test_unknown_solver_is_rejected(self):
        freqs, mag = make_flat_response()
        with pytest.raises(ValueError, match="unknown solver"):
            compute_correction(freqs, mag, solver="magic")

    def test_solve_cascade_gains_degenerate_inputs(self):
        grid = np.logspace(np.log10(20), np.log10(20000), 64)
        target = np.zeros_like(grid)
        assert solve_cascade_gains(grid, target, []).shape == (0,)
        # max_gain_db of 0 means "no correction permitted", not a solver error.
        np.testing.assert_array_equal(
            solve_cascade_gains(grid, target + 5.0, DEFAULT_BAND_HZ, max_gain_db=0.0),
            np.zeros(10))

    def test_q_changes_the_solution(self):
        """Q is now an input to the gains, not only to the preamp headroom."""
        freqs, room = self.room([(125.0, 8.0), (2000.0, -6.0)])
        g1, _ = compute_correction(freqs, room, q=1.0, auto_preamp=False)
        g3, _ = compute_correction(freqs, room, q=3.0, auto_preamp=False)
        assert not np.allclose(g1, g3, atol=0.1)


class TestOctaveNormalisation:

    def test_median_is_zero_after_normalisation(self):
        gains = np.array([3.0, -2.0, 1.0, 4.0, -1.0, 0.0, 2.0, -3.0, 1.0, 0.5])
        normalised = apply_octave_normalisation(DEFAULT_BAND_HZ, gains)
        assert abs(np.median(normalised)) < 1e-9
