"""
loaders.py — Pluggable measurement-file loader registry.

Right now this project only reads WAV recordings, via measurement.load_wav
(Welch PSD estimate) and measurement.load_impulse_response (windowed FFT of
an impulse response). This module exists specifically so *other*
measurement file formats -- e.g. a different measurement tool's own export
format -- can be added later without changing visualize.py, cli.py, or
anything else that loads a measurement: register a new loader here and it
becomes selectable everywhere via a format name / file extension.

This intentionally does NOT implement a second format yet -- only the
extension point. Wire up a real second format by following the pattern
below once its exact structure (column layout, units, header lines, etc.)
is known; guessing at an unfamiliar format's details would risk silently
mis-reading real measurement data.

Design
------
A loader is any callable with the signature:

    loader(path: str, channel: int = 0, ir: bool = False) -> (freqs, magnitude_db, sample_rate)

matching measurement.load_wav's return shape:
    freqs        : np.ndarray, frequency axis in Hz
    magnitude_db : np.ndarray, magnitude in dB (same shape as freqs)
    sample_rate  : float, or None if the format doesn't carry one

`sample_rate` is allowed to be None for formats that only ever store a
frequency/magnitude table (e.g. a plain text frequency-response export) --
visualize.py falls back to a default when synthesising the continuous EQ
curve, since that value only affects where the Nyquist-adjacent edge of the
plotted frequency grid falls, not the measured data itself.

To add a new format
--------------------
    1. Write a function matching the signature above.
    2. Call register_loader("myformat", my_loader_fn, extensions=[".ext"]).
    3. It's now selectable via --input-format/--output-format myformat on
       the `eq-curvegen visualize` CLI, and auto-detected from the file's
       extension when no format is given explicitly.
"""
from __future__ import annotations

import os
from typing import Callable, Dict, List, Optional, Sequence, Tuple

import numpy as np

from . import measurement

FreqResponse = Tuple[np.ndarray, np.ndarray, Optional[float]]
LoaderFn = Callable[..., FreqResponse]

_LOADERS: Dict[str, LoaderFn] = {}
_EXTENSIONS: Dict[str, str] = {}  # lowercase extension (with dot) -> format name


def register_loader(name: str, fn: LoaderFn, extensions: Sequence[str] = ()) -> None:
    """
    Register a measurement-file loader under `name`. Re-registering an
    existing name overwrites it (useful for tests, or for a user
    intentionally overriding a built-in loader).

    `extensions` (e.g. [".txt"]) drive auto-detection from a file path when
    the caller doesn't pass an explicit format. If an extension is already
    claimed by another format, the new registration wins -- last one in.
    """
    _LOADERS[name] = fn
    for ext in extensions:
        _EXTENSIONS[ext.lower()] = name


def available_formats() -> List[str]:
    return sorted(_LOADERS.keys())


def detect_format(path: str) -> Optional[str]:
    """Guess a registered format name from `path`'s file extension, or None."""
    ext = os.path.splitext(path)[1].lower()
    return _EXTENSIONS.get(ext)


def load(
    path: str,
    fmt: Optional[str] = None,
    channel: int = 0,
    ir: bool = False,
) -> FreqResponse:
    """
    Load a measurement file, returning (freqs, magnitude_db, sample_rate).

    `fmt` selects a registered loader by name explicitly. If omitted, the
    format is guessed from the file's extension, falling back to "wav" if
    that also fails to resolve (matching this project's only currently
    supported format).
    """
    resolved = fmt or detect_format(path) or "wav"
    if resolved not in _LOADERS:
        raise ValueError(
            f"Unknown measurement format {resolved!r}. "
            f"Available: {available_formats()}. "
            "See curvegen/loaders.py's module docstring to register a new one."
        )
    return _LOADERS[resolved](path, channel=channel, ir=ir)


# ── Built-in loader: WAV / impulse-response WAV ──────────────────────────────
# Thin wrapper around the existing measurement.py functions so they're
# reachable through the same registry every other format will use.

def _load_wav_format(path: str, channel: int = 0, ir: bool = False) -> FreqResponse:
    if ir:
        freqs, mag_db, sr = measurement.load_impulse_response(path, channel)
    else:
        freqs, mag_db, sr = measurement.load_wav(path, channel)
    return freqs, mag_db, float(sr)


register_loader("wav", _load_wav_format, extensions=[".wav", ".wave"])
