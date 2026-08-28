"""
visualize.py — Build a 4-stage room-correction validation report.

Stages
------
1. Recorded input   — the raw room measurement, before correction.
2. Curve generated  — the actual continuous frequency response of the
                       10-band peaking EQ (+ preamp) that flatten.py
                       computed from stage 1, evaluated with the *exact*
                       RBJ formula DSP::Biquad::SetPeaking uses (see
                       DSP/Biquad.cpp and evaluate_eq_response_db() below)
                       — not a straight-line interpolation between the 10
                       band points. Cascaded bands combine correctly (dB
                       magnitudes add for filters in series), so this
                       curve reflects real overlap/interaction between
                       adjacent bands, the same way the real DSP chain
                       would produce it.
3. Expected output  — stage 1 + stage 2 (+ preamp): the response a
                       faithful implementation of this curve *should*
                       produce, computed mathematically, before physically
                       re-measuring anything.
4. Recorded output  — a second real measurement, taken after physically
                       applying the correction (e.g. via the offline
                       Equalizer APO export in eqapo_export.py, or via
                       eq-daemon), for comparison against stage 3. Optional:
                       if it hasn't been taken yet, this stage is reported
                       as unavailable rather than raising an error.

Each stage is analysed two ways:
  - FFT : the full-resolution curve — raw Welch/FFT bins for the two
          *measured* stages (1 and 4), or the dense analytic filter
          response for the two *computed* stages (2 and 3).
  - CPB : the same curve after fractional-octave ("Constant Percentage
          Bandwidth") smoothing, via measurement.smooth_octave() — 1/3
          octave by default, the standard acoustic-measurement convention.
          Applying the identical smoothing function to all four stages
          (rather than treating stage 2's CPB view as just "the 10 band
          points") keeps "FFT" and "CPB" meaning the same thing everywhere
          in this report.

Input formats
-------------
Stages 1 and 4 are loaded through curvegen.loaders, a small pluggable
registry — not hardcoded to WAV — specifically so support for other
measurement file formats can be added later without touching this module.
See curvegen/loaders.py's module docstring for how to register a new one.
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import List, Optional, Sequence, Union

import numpy as np

from . import flatten
from . import loaders
from . import measurement

CPB_FRACTION = 1.0 / 3.0  # 1/3-octave, matches measurement.smooth_octave's default
FFT_GRID_POINTS = 500
FFT_GRID_MIN_HZ = 20.0
FFT_GRID_MAX_HZ = 20000.0
DEFAULT_SAMPLE_RATE = 48000.0  # fallback only; see loaders.py's sample_rate note

# Fractional-octave smoothing applied before computing the correction curve.
# Must match cli._analyse()'s value, which is what `measure` and `eqapo` use --
# this is the analysis setting, distinct from CPB_FRACTION, which only affects
# how the report is drawn.
ANALYSIS_FRACTION = 1.0 / 3.0


# ── RBJ peaking-biquad response ────────────────────────────────────
#
# These live in curvegen/response.py now: flatten.py needs the same cascade
# math for its auto-preamp headroom calculation, and it cannot import
# visualize (visualize imports flatten). Re-exported here so
# `visualize.evaluate_eq_response_db(...)` keeps working for existing
# callers and tests.
from .response import (  # noqa: F401
    _peaking_biquad_coeffs,
    _biquad_magnitude_db,
    evaluate_eq_response_db,
    cascade_peak_db,
)


def synthetic_freq_grid(sample_rate: float, points: int = FFT_GRID_POINTS) -> np.ndarray:
    """Dense log-spaced frequency grid for evaluating a continuous (non-measured) curve."""
    sr = sample_rate if sample_rate and sample_rate > 0 else DEFAULT_SAMPLE_RATE
    fmax = min(FFT_GRID_MAX_HZ, sr / 2.0 * 0.99)
    return np.logspace(np.log10(FFT_GRID_MIN_HZ), np.log10(fmax), points)


# ── Stage / report data model ────────────────────────────────────────────────

@dataclass
class StageCurve:
    """One stage's FFT-resolution and CPB-smoothed representations."""
    name: str
    freqs: Optional[np.ndarray] = None
    fft_db: Optional[np.ndarray] = None
    cpb_freqs: Optional[np.ndarray] = None
    cpb_db: Optional[np.ndarray] = None
    available: bool = True
    note: str = ""


@dataclass
class Report:
    recorded_input: StageCurve
    curve_generated: StageCurve
    expected_output: StageCurve
    recorded_output: StageCurve
    band_hz: List[float]
    gains_db: np.ndarray
    preamp_db: float
    q: Union[float, Sequence[float]]

    @property
    def stages(self) -> List[StageCurve]:
        return [self.recorded_input, self.curve_generated, self.expected_output, self.recorded_output]


def build_report(
    recorded_input_path: str,
    recorded_output_path: Optional[str] = None,
    input_format: Optional[str] = None,
    output_format: Optional[str] = None,
    input_channel: int = 0,
    output_channel: int = 0,
    ir: bool = False,
    harman: bool = False,
    max_gain_db: float = flatten.MAX_GAIN_DB,
    q: Union[float, Sequence[float]] = 1.0,
    cpb_fraction: float = CPB_FRACTION,
) -> Report:
    """
    Run the full 4-stage analysis. `recorded_output_path` is optional --
    pass it once you have a post-correction measurement; until then, stage 4
    is reported as unavailable rather than raising an error, since it's
    entirely reasonable to want stages 1-3 (input, curve, prediction) before
    the physical re-measurement has happened.
    """
    # Stage 1: recorded input
    freqs1, mag1, sr1 = loaders.load(recorded_input_path, fmt=input_format, channel=input_channel, ir=ir)
    cpb_freqs1, cpb_mag1 = measurement.smooth_octave(freqs1, mag1, fraction=cpb_fraction)
    stage1 = StageCurve("Recorded input", freqs1, mag1, cpb_freqs1, cpb_mag1)

    # Stage 2: curve generated.
    #
    # The correction MUST be derived from the same input `measure`/`eqapo`
    # derive it from, or this report validates something the rest of the tool
    # never produces. cli._analyse() smooths at a fixed 1/3 octave before
    # calling compute_correction(); this used to pass the raw, unsmoothed
    # mag1, so the "curve generated" panel showed a different curve than the
    # one a `measure` run on the same file would write out.
    #
    # Note this deliberately uses ANALYSIS_FRACTION, not `cpb_fraction`:
    # cpb_fraction only controls how wide the display smoothing is, and
    # changing a view setting must not change the curve being validated.
    _, analysis_mag1 = measurement.smooth_octave(freqs1, mag1, fraction=ANALYSIS_FRACTION)
    gains_db, preamp_db = flatten.compute_correction(
        freqs1, analysis_mag1, max_gain_db=max_gain_db, use_harman_target=harman,
    )
    band_hz = flatten.DEFAULT_BAND_HZ
    sr_for_eval = sr1 if sr1 else DEFAULT_SAMPLE_RATE
    grid = synthetic_freq_grid(sr_for_eval)
    filter_db = evaluate_eq_response_db(grid, band_hz, gains_db, q=q, sample_rate=sr_for_eval)
    curve_db = filter_db + preamp_db
    cpb_freqs2, cpb_curve2 = measurement.smooth_octave(grid, curve_db, fraction=cpb_fraction)
    stage2 = StageCurve("Curve generated", grid, curve_db, cpb_freqs2, cpb_curve2)

    # Stage 3: expected output = stage 1 (interpolated onto the same grid) + filter + preamp
    log_freqs1 = np.log10(np.maximum(freqs1, 1e-6))
    interp_input_db = np.interp(np.log10(grid), log_freqs1, mag1)
    expected_db = interp_input_db + filter_db + preamp_db
    cpb_freqs3, cpb_expected3 = measurement.smooth_octave(grid, expected_db, fraction=cpb_fraction)
    stage3 = StageCurve("Expected output", grid, expected_db, cpb_freqs3, cpb_expected3)

    # Stage 4: recorded output (optional)
    if recorded_output_path:
        freqs4, mag4, sr4 = loaders.load(recorded_output_path, fmt=output_format, channel=output_channel, ir=ir)
        cpb_freqs4, cpb_mag4 = measurement.smooth_octave(freqs4, mag4, fraction=cpb_fraction)
        stage4 = StageCurve("Recorded output", freqs4, mag4, cpb_freqs4, cpb_mag4)
    else:
        stage4 = StageCurve(
            "Recorded output", available=False,
            note="No second measurement supplied yet.\nPass --recorded-output once you've\n"
                 "applied the correction and re-measured.",
        )

    return Report(stage1, stage2, stage3, stage4, list(band_hz), gains_db, preamp_db, q)


# ── Plotting ──────────────────────────────────────────────────────────────────

def plot_report(report: Report, output_path: str, cpb_fraction: float = CPB_FRACTION) -> None:
    """
    Render `report` as a 2x2 matplotlib figure (one panel per stage) and
    save it to `output_path`. Each panel overlays the FFT-resolution curve
    (faint) and the CPB/octave-smoothed curve (bold). The "Curve generated"
    panel also marks the actual discrete band gains as points, since those
    are the literal values the DSP would apply. The "Recorded output" panel
    overlays a faint dashed copy of "Expected output"'s CPB curve for direct
    predicted-vs-actual comparison, if available.
    """
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(2, 2, figsize=(13, 9), sharex=False)
    stage_axes = {
        "Recorded input":   axes[0][0],
        "Curve generated":  axes[0][1],
        "Expected output":  axes[1][0],
        "Recorded output":  axes[1][1],
    }

    cpb_label = f"CPB (1/{round(1/cpb_fraction)} oct)" if cpb_fraction > 0 else "CPB"

    for stage in report.stages:
        ax = stage_axes[stage.name]
        if not stage.available:
            ax.text(0.5, 0.5, stage.note or "Not available", ha="center", va="center",
                     transform=ax.transAxes, fontsize=10, color="gray", wrap=True)
            ax.set_title(stage.name)
            ax.set_xticks([])
            ax.set_yticks([])
            continue

        ax.semilogx(stage.freqs, stage.fft_db, alpha=0.35, label="FFT", color="tab:blue")
        ax.semilogx(stage.cpb_freqs, stage.cpb_db, label=cpb_label, color="tab:blue", linewidth=2)

        if stage.name == "Curve generated":
            # These are the *requested* per-band gains fed into
            # SetBandsPeaking -- i.e. what flatten.py asked for at each
            # band's own center frequency in isolation. They are NOT
            # necessarily identical to the solid curve directly above/below
            # them: with 10 bands only an octave or so apart, neighbouring
            # bands' skirts overlap and add (in dB) at any shared
            # frequency, so the actual cascaded response the DSP produces
            # (the solid line) can differ from a single band's own
            # requested gain at that same frequency. Showing both is the
            # point -- it's a faithful picture of real filter interaction,
            # not a discrepancy to paper over.
            requested_gains = np.asarray(report.gains_db, dtype=float) + report.preamp_db
            ax.scatter(report.band_hz, requested_gains, color="tab:orange", zorder=5,
                       label="Requested band gains", marker="o")

        if stage.name == "Recorded output" and report.expected_output.available:
            ax.semilogx(report.expected_output.cpb_freqs, report.expected_output.cpb_db,
                        linestyle="--", color="tab:red", alpha=0.7, label="Expected (predicted)")

        ax.axhline(0, color="k", linewidth=0.6, alpha=0.5)
        ax.set_title(stage.name)
        ax.set_xlabel("Frequency (Hz)")
        ax.set_ylabel("Magnitude (dB)")
        ax.grid(True, which="both", linestyle="--", alpha=0.4)
        ax.legend(fontsize=8)

    fig.suptitle("Room correction validation report", fontsize=14)
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    fig.savefig(output_path, dpi=150)
    plt.close(fig)
