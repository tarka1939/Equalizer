"""
flatten.py — Compute a correction EQ curve that "flattens" a measured
             frequency response.

Algorithm
---------
1. Build the *target* correction curve: invert the measured response,
   reference it to 0 dB at 1 kHz, and optionally blend toward a
   psychoacoustic target (Harman 2018).
2. Solve for the band gains that make the **cascade** of peaking filters
   match that target as closely as possible, subject to |gain| <= max_gain_db
   (`solve_cascade_gains`).
3. Optionally compute a preamp so the cascade's real worst-case boost does
   not clip.

Why step 2 is a solve and not an inversion
------------------------------------------
The obvious implementation -- and what this module did until the cascade
solver landed -- is `gain[i] = -measured(band_hz[i])`: read the measured
response at each band centre and invert it, one band at a time. That is only
correct if the bands are independent, and they are not. They are a *cascade*
of peaking biquads, and at the default Q of 1.0 each is roughly an octave
wide while the centres sit an octave apart, so every band's correction leaks
into its neighbours. The gain a band asks for is not the gain the chain
delivers.

Measured on a real room (see `TEST_RESULTS.md`): a -6.85 dB correction at
62 Hz pulled 125 Hz down an extra 2 dB, so a band that measured +1.40 dB
before correction came out at -3.16 dB after it -- an already-flat band made
worse by its neighbour. Iterating relocated the error rather than removing
it. On the closed-form synthetic room of `TEST_PLAN.md` Phase 2, pointwise
inversion leaves 2.27 dB RMS residual where the solver leaves 0.11 dB.

The fix is to solve for the whole gain vector at once against the cascade's
actual response. See `solve_cascade_gains`.

The auto-preamp calculation was already cascade-aware
(`response.cascade_peak_db`); this brings the gain solver into line with it.
"""
from __future__ import annotations

import warnings

import numpy as np
from scipy.optimize import lsq_linear
from typing import Optional, Sequence, Tuple

from .response import DEFAULT_SAMPLE_RATE, cascade_peak_db, evaluate_eq_response_db

# ── Constants ─────────────────────────────────────────────────────────────────

DEFAULT_BAND_HZ: list[float] = [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]
MAX_GAIN_DB: float = 12.0    # hard per-band limit
MIN_FREQ_HZ: float = 20.0    # ignore below this (unreliable measurements)

# Solver defaults. GRID_POINTS is the log-spaced frequency grid the cascade is
# fitted on; REFINE_ITERATIONS is the Gauss-Newton refinement count (see
# solve_cascade_gains). Both were chosen empirically: the fit is converged by
# 4 iterations (a 5th changes the result by <0.001 dB) and denser grids do not
# improve it.
GRID_POINTS: int = 256
REFINE_ITERATIONS: int = 4

# Tikhonov damping on the least-squares solve. Irrelevant for the default
# 10 bands, which are well conditioned; it matters for denser band sets, where
# heavily overlapping filters make the basis near-singular and an undamped
# solve can return a pair of huge opposing gains that happen to cancel.
SOLVER_DAMPING: float = 1e-3

# Harman 2018 in-room target (deviation from flat in dB, referenced to 1 kHz).
# Values at: 20, 40, 80, 160, 315, 630, 1250, 2500, 5000, 10000, 20000 Hz
_HARMAN_HZ  = np.array([20,  40,  80, 160, 315, 630, 1250, 2500, 5000, 10000, 20000], dtype=float)
_HARMAN_DB  = np.array([3.0, 4.0, 3.5, 2.0, 1.0, 0.5,  0.0, -1.0, -3.0, -5.0,  -9.0])


# ── Target curve and cascade solver ───────────────────────────────────────────

def _target_curve(
    at_hz: np.ndarray,
    freqs: np.ndarray,
    magnitude_db: np.ndarray,
    use_harman_target: bool,
    harman_blend: float,
) -> np.ndarray:
    """
    The correction curve we *want* the EQ to produce, in dB, evaluated at
    `at_hz`. Independent of how many filters exist or what shape they are --
    that is the solver's problem, not this function's.

        target(f) = level(1 kHz) - level(f) + blend * harman(f)

    Referencing to 1 kHz is a convention, not a measurement: `measurement.py`
    reports an uncalibrated scale (Welch PSD units or raw FFT magnitude, not
    dBFS or SPL), so "0 dB" is only meaningful relative to some chosen anchor.
    1 kHz is the standard one. The cost is that a defect located exactly at
    1 kHz is invisible by construction -- see ARCHITECTURE.md section 7.4 and
    `test_boost_at_reference_frequency_is_self_cancelling`.

    The Harman term folds out of the original blend algebra exactly:
    `(1-b)*(-m + r) + b*(-m + r + h)` == `-m + r + b*h`.
    """
    log_freqs = np.log10(np.maximum(freqs, 1e-6))
    ref_1khz_db = np.interp(np.log10(1000.0), log_freqs, magnitude_db)
    target = ref_1khz_db - np.interp(np.log10(at_hz), log_freqs, magnitude_db)

    if use_harman_target:
        blend = float(np.clip(harman_blend, 0.0, 1.0))
        target = target + blend * np.interp(at_hz, _HARMAN_HZ, _HARMAN_DB)

    return target


def solve_cascade_gains(
    target_hz: np.ndarray,
    target_db: np.ndarray,
    band_hz: Sequence[float],
    q: float = 1.0,
    sample_rate: float = DEFAULT_SAMPLE_RATE,
    max_gain_db: float = MAX_GAIN_DB,
    grid_points: int = GRID_POINTS,
    refine_iterations: int = REFINE_ITERATIONS,
    damping: float = SOLVER_DAMPING,
) -> np.ndarray:
    """
    Find the per-band gains whose *cascaded* response best matches `target_db`.

    Solves, over a log-spaced frequency grid,

        minimise || A @ g - target ||^2 + damping * ||g||^2
        subject to |g[j]| <= max_gain_db

    where ``A[i][j]`` is the dB contribution at grid frequency ``i`` of band
    ``j`` driven at unit gain. Cascaded biquads multiply in linear magnitude
    and so add in dB (`evaluate_eq_response_db`), which is what makes the
    problem linear in `g` and lets a bounded least-squares solve it directly.

    **The linearity is not quite exact**, which is why this iterates. A
    peaking filter's *skirt* shape depends mildly on its own gain -- ``A`` is
    the derivative at unit gain, not a gain-independent basis -- so a
    single-shot solve is slightly off. Each iteration evaluates the true
    cascade at the current gains and solves for a correction to the residual,
    i.e. Gauss-Newton with a fixed Jacobian. It converges quickly: on a real
    measurement, iteration 1 leaves 0.615 dB RMS, iteration 2 leaves 0.568,
    iteration 4 leaves 0.564, and further iterations change nothing at three
    decimal places. Notably the converged result is *better* than the exact
    unconstrained solve of the single-shot linear system (0.612 dB), which is
    the clearest evidence the refinement is correcting real nonlinearity
    rather than just polishing round-off.

    Bounds are applied per iteration against the accumulated gain, so the
    result satisfies them exactly rather than being clipped at the end
    (clipping a converged solve would silently discard the fit).

    Returns an array of `len(band_hz)` gains in dB.
    """
    bands = np.asarray(band_hz, dtype=float)
    n = len(bands)
    if n == 0:
        return np.zeros(0)

    max_gain_db = float(abs(max_gain_db))
    if max_gain_db == 0.0:
        # Degenerate but legitimate ("apply no correction"). lsq_linear rejects
        # equal lower/upper bounds, so short-circuit rather than fail.
        return np.zeros(n)

    sr = sample_rate if sample_rate and sample_rate > 0 else DEFAULT_SAMPLE_RATE

    # Fit only where the measurement actually has data. np.interp clamps to the
    # endpoint value outside its range, so a grid extending past the measured
    # band would fit the cascade to a flat extrapolation and confidently
    # "correct" it. compute_correction warns separately about band centres in
    # that region; this keeps the fit itself honest.
    target_hz = np.asarray(target_hz, dtype=float)
    f_lo = max(MIN_FREQ_HZ, float(np.min(target_hz)))
    f_hi = min(20000.0, sr / 2.0 * 0.99, float(np.max(target_hz)))
    if not (f_hi > f_lo):
        return np.zeros(n)

    grid = np.logspace(np.log10(f_lo), np.log10(f_hi), max(int(grid_points), n * 4))
    target = np.interp(np.log10(grid), np.log10(target_hz), np.asarray(target_db, dtype=float))

    # Unit-gain basis. Driving at 1 dB and dividing keeps A in "dB of response
    # per dB of band gain", so `damping` is scale-free.
    basis = np.column_stack([
        evaluate_eq_response_db(grid, [fc], [1.0], q=q, sample_rate=sr) for fc in bands
    ])
    reg = np.sqrt(max(damping, 0.0)) * np.eye(n)
    design = np.vstack([basis, reg])

    gains = np.zeros(n)
    for _ in range(max(int(refine_iterations), 1)):
        actual = evaluate_eq_response_db(grid, bands, gains, q=q, sample_rate=sr)
        rhs = np.concatenate([target - actual, np.zeros(n)])
        step = lsq_linear(
            design, rhs,
            bounds=(-max_gain_db - gains, max_gain_db - gains),
            method="trf", tol=1e-10,
        ).x
        gains = np.clip(gains + step, -max_gain_db, max_gain_db)

    return gains


# ── Public API ────────────────────────────────────────────────────────────────

def compute_correction(
    freqs: np.ndarray,
    magnitude_db: np.ndarray,
    band_hz: Optional[Sequence[float]] = None,
    max_gain_db: float = MAX_GAIN_DB,
    use_harman_target: bool = False,
    harman_blend: float = 0.5,
    auto_preamp: bool = True,
    q: float = 1.0,
    sample_rate: float = DEFAULT_SAMPLE_RATE,
    solver: str = "cascade",
) -> Tuple[np.ndarray, float]:
    """
    Compute per-band correction gains that flatten the measured response.

    Parameters
    ----------
    freqs, magnitude_db  : measured frequency response (from measurement.py).
                           Note measurement.py's warning: these are raw
                           spectra of the recording, not deconvolved room
                           transfer functions.
    band_hz              : centre frequencies of the EQ bands (default 10-band)
    max_gain_db          : maximum allowed gain magnitude per band
    use_harman_target    : blend toward Harman 2018 target instead of flat
    harman_blend         : 0 = pure flat target, 1 = pure Harman target
    auto_preamp          : if True, return a negative preamp to avoid clipping
    q                    : Q the playback chain will use for every band. It
                           must match what actually gets applied: with
                           solver="cascade" it determines how much each band
                           overlaps its neighbours and therefore the gains
                           themselves, and it feeds the auto-preamp headroom
                           calculation either way.
    sample_rate          : sample rate the filters will run at; used for the
                           same two purposes as `q`.
    solver               : "cascade" (default) solves for all gains at once
                           against the cascade's real response, so overlapping
                           bands are accounted for. "pointwise" is the old
                           per-band inversion, kept for comparison and
                           regression testing only -- it is wrong by up to
                           ~3 dB on real measurements and actively damages
                           already-flat bands next to large corrections. See
                           this module's docstring and `solve_cascade_gains`.

    Returns
    -------
    gains_db   : np.ndarray shape (N,) — one value per band
    preamp_db  : float — suggested global preamp (≤ 0 dB)
    """
    if band_hz is None:
        band_hz = DEFAULT_BAND_HZ
    bands = np.asarray(band_hz, dtype=float)

    # np.interp clamps to the endpoint value outside the measured range rather
    # than extrapolating or erroring. That is silently wrong for bands the
    # measurement never covered -- e.g. the 16 kHz band against a 32 kHz-rate
    # recording gets whatever the response happened to be at Nyquist, and a
    # correction is then confidently generated from it. Warn instead of
    # letting it pass unnoticed.
    usable = freqs[freqs >= MIN_FREQ_HZ]
    f_min = float(usable.min()) if usable.size else MIN_FREQ_HZ
    f_max = float(freqs.max()) if np.size(freqs) else MIN_FREQ_HZ
    out_of_range = bands[(bands < f_min) | (bands > f_max)]
    if out_of_range.size:
        warnings.warn(
            "Band centre(s) "
            + ", ".join(f"{b:g} Hz" for b in out_of_range)
            + f" lie outside the measured range [{f_min:g}, {f_max:g}] Hz; "
              "their correction is extrapolated from the nearest measured bin "
              "and should not be trusted.",
            UserWarning,
            stacklevel=2,
        )

    sr = float(sample_rate) if sample_rate else DEFAULT_SAMPLE_RATE

    if solver == "pointwise":
        # Legacy per-band inversion: read the target at each band centre and
        # ask for exactly that. Wrong whenever the bands overlap -- see this
        # module's docstring. Kept so the defect stays reproducible.
        gains_db = np.clip(
            _target_curve(bands, freqs, magnitude_db, use_harman_target, harman_blend),
            -max_gain_db, max_gain_db,
        )
    elif solver == "cascade":
        # Solve for the gain vector whose cascaded response matches the target
        # curve, evaluated across the measured range rather than at ten points.
        usable_mask = freqs >= MIN_FREQ_HZ
        target_hz = freqs[usable_mask] if np.any(usable_mask) else freqs
        gains_db = solve_cascade_gains(
            target_hz,
            _target_curve(target_hz, freqs, magnitude_db, use_harman_target, harman_blend),
            bands, q=q, sample_rate=sr, max_gain_db=max_gain_db,
        )
    else:
        raise ValueError(f"unknown solver {solver!r} (expected 'cascade' or 'pointwise')")

    # Preamp: headroom so the filter chain's actual peak doesn't clip.
    #
    # This used to be -max(gains_db), i.e. the largest single band gain. That
    # under-estimates the headroom needed, because the bands are a *cascade*
    # of peaking biquads that overlap: at the default Q of 1.0 each band is
    # roughly an octave wide while the centres are an octave apart, so two
    # adjacent boosts sum and the combined response between them exceeds
    # either one alone. The daemon's output clamp then hard-clips exactly the
    # difference. cascade_peak_db() evaluates the real summed response.
    preamp_db = 0.0
    if auto_preamp:
        peak_db = cascade_peak_db(bands, gains_db, q=q, sample_rate=sr)
        if peak_db > 0:
            preamp_db = -peak_db

    return gains_db, preamp_db


def apply_octave_normalisation(
    band_hz: Sequence[float],
    gains_db: np.ndarray,
) -> np.ndarray:
    """
    Normalise so the median band gain is 0 dB (centres the curve).
    Useful when you want perceptual levelling rather than absolute correction.
    """
    return gains_db - np.median(gains_db)
