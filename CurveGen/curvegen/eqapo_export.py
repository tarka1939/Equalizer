"""
eqapo_export.py — Write an Equalizer APO config file from computed
correction gains, for OFFLINE real-world validation of the curve-generation
algorithm.

Why this exists
----------------
Equalizer APO (https://equalizerapo.com, https://sourceforge.net/projects/equalizerapo/)
is a well-established, independently implemented, widely-used Windows
system-wide equalizer with a plain-text config format. Writing that format
lets a room-correction curve produced by this project's own measurement +
flatten.py pipeline be dropped straight into a known-good, already-verified
third-party engine and actually listened to (or re-measured) on real
hardware, in real rooms -- without depending on any of this project's own
audio-hosting code, which currently has open gaps for exactly that purpose:

  - the in-repo Windows APO (`Equalizer/Equalizer.cpp`) has a bug where the
    band curve configured in `LockForProcess()` is never applied by
    `APOProcess()` (see ARCHITECTURE.md §7.1) -- so it can't currently be
    trusted to audibly reflect a generated curve at all;
  - `eq-daemon` doesn't build on Windows (§7.3), and the Windows IPC path is
    a stub on both the daemon and GUI sides (§7.2).

This module is a pure *writer*. It does not read Equalizer APO's config
format back in, and it has nothing to do with running this project's own
DSP -- it exists purely so the curve-generation *math* (measurement.py +
flatten.py) can be validated end-to-end against real playback, decoupled
from whether this project's own APO/daemon happen to work today.

Config file format
-------------------
Verified against the official Equalizer APO documentation (not assumed):
https://sourceforge.net/p/equalizerapo/wiki/Configuration%20reference/

    Preamp: <value> dB
    Filter <n>: ON PK Fc <frequency> Hz Gain <gain> dB Q <q>

"PK" is Equalizer APO's peaking filter ("Parametric EQ") -- the same RBJ
peaking-biquad shape that `DSP::Biquad`/`DSP::Equalizer10Band` implement
(see ARCHITECTURE.md §2.1), so this is a faithful match for this project's
own filter shape rather than an approximation using a different filter
type. Filter numbers are not interpreted by Equalizer APO (the docs state
they "can be omitted") and are emitted 1-indexed purely for human
readability of the generated file.
"""
from __future__ import annotations

import os
from typing import Sequence, Union

from .flatten import DEFAULT_BAND_HZ

# Matches shared/preset_schema.json's documented default Q ("Optional Q
# factor override for this band (default: 1.0)"), so a curve exported here
# and one exported as a JSON preset represent the same filter shape unless
# the caller explicitly overrides Q.
DEFAULT_Q = 1.0


def _format_hz(hz: float) -> str:
    """
    Format a frequency for the config file, dropping unnecessary trailing
    zeros (1000.0 -> "1000", 31.25 -> "31.25"). Equalizer APO's own examples
    use both "50 Hz" and "50.0 Hz" forms, so either is valid; this just
    keeps whole-number band centres (the common case) readable.
    """
    s = f"{float(hz):.3f}"
    if "." in s:
        s = s.rstrip("0").rstrip(".")
    return s


def render_eqapo_config(
    gains_db: Sequence[float],
    band_hz: Sequence[float] = None,
    preamp_db: float = 0.0,
    q: Union[float, Sequence[float]] = DEFAULT_Q,
    comment: str = "",
) -> str:
    """
    Render an Equalizer APO config file as a string.

    Parameters
    ----------
    gains_db  : per-band gains in dB
    band_hz   : band centre frequencies in Hz (default: standard 10-band)
    preamp_db : global preamp in dB, emitted as a `Preamp:` line ahead of
                the filters (Equalizer APO applies Preamp before Filter
                commands regardless of source-line order, but placing it
                first matches every example in the official docs)
    q         : a single Q shared by every band, or a per-band sequence the
                same length as band_hz/gains_db
    comment   : optional free-text header, emitted as leading "# " comment
                lines (Equalizer APO ignores any line not matching a known
                command, so plain "#"-prefixed lines are safe, matching the
                comment style used in the docs' own examples)

    Returns
    -------
    The full config file contents as a single string (trailing newline
    included).
    """
    if band_hz is None:
        band_hz = DEFAULT_BAND_HZ
    if len(band_hz) != len(gains_db):
        raise ValueError(
            f"band_hz length ({len(band_hz)}) must match gains_db length ({len(gains_db)})"
        )

    if isinstance(q, (int, float)):
        q_values = [float(q)] * len(band_hz)
    else:
        q_values = [float(v) for v in q]
        if len(q_values) != len(band_hz):
            raise ValueError(
                f"q length ({len(q_values)}) must match band_hz length ({len(band_hz)})"
            )
    if any(v <= 0 for v in q_values):
        raise ValueError("Q must be strictly positive for every band")

    lines: list[str] = []
    if comment:
        for line in comment.splitlines():
            lines.append(f"# {line}" if line else "#")
        lines.append("")

    lines.append(f"Preamp: {float(preamp_db):.2f} dB")
    lines.append("")

    for i, (hz, gain, qv) in enumerate(zip(band_hz, gains_db, q_values), start=1):
        lines.append(
            f"Filter {i}: ON PK Fc {_format_hz(hz)} Hz "
            f"Gain {float(gain):.2f} dB Q {qv:.2f}"
        )

    return "\n".join(lines) + "\n"


def write_eqapo_config(
    path: str,
    gains_db: Sequence[float],
    band_hz: Sequence[float] = None,
    preamp_db: float = 0.0,
    q: Union[float, Sequence[float]] = DEFAULT_Q,
    comment: str = "",
) -> None:
    """
    Render and write an Equalizer APO config file to `path`.

    To use the result: install Equalizer APO (https://equalizerapo.com),
    then either paste this file's contents into its main `config.txt` (via
    the "Configurator" or a text editor) or reference it with an
    `Include: <path>` line from `config.txt`.
    """
    text = render_eqapo_config(gains_db, band_hz, preamp_db, q, comment)

    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)

    print(f"[eqapo_export] Equalizer APO config saved to {path}")
