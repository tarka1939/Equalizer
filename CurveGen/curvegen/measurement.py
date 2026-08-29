"""
measurement.py — Load and analyse audio measurements for room correction.

Supported inputs:
  - ``load_wav``: any WAV recording (mono or stereo; one channel selected).
    Returns the Welch PSD of the recording as-is.
  - ``load_impulse_response``: a WAV already containing an impulse response
    (e.g. exported from REW). Windows it and takes the magnitude spectrum.

All functions return:
    freqs        : np.ndarray of shape (N,)  — frequency axis in Hz
    magnitude_db : np.ndarray of shape (N,)  — average magnitude in dB
    sample_rate  : int

.. warning::

   **These are raw spectra of the recording, not room transfer functions.**
   Neither loader divides out the excitation signal, so whatever spectrum the
   test signal itself had is baked into the result -- and ``flatten.py`` will
   dutifully "correct" it.

   Concretely: pink noise falls at -3 dB/octave by definition. Measure a
   perfectly flat room with pink noise and ``load_wav`` reports a -3 dB/octave
   slope, which inverts into a +3 dB/octave boost. The generated curve is
   audibly wrong (very bright), and nothing in the current pipeline detects it.

   Until a reference-aware path exists, the usable inputs are:
     - an impulse response measured elsewhere (REW etc.), via
       ``load_impulse_response``, which is already deconvolved; or
     - a ``load_wav`` measurement of a signal that is flat by construction
       (white noise), accepting that this is a poor room-measurement stimulus.

   Not yet implemented, despite earlier versions of this docstring claiming
   otherwise: swept-sine (logarithmic sweep) deconvolution, and pink-noise RTA
   with the -3 dB/octave reference removed. There is no deconvolution code in
   this module at all.
"""
from __future__ import annotations

import numpy as np
from scipy.io import wavfile
from scipy.signal import welch, get_window
from typing import Tuple


FreqResponse = Tuple[np.ndarray, np.ndarray, int]


# ── Public API ────────────────────────────────────────────────────────────────

def load_wav(path: str, channel: int = 0) -> FreqResponse:
    """
    Load a WAV file and compute its magnitude frequency response via Welch's
    power spectral density estimate.

    Parameters
    ----------
    path    : path to WAV file (16-bit int, 24-bit int, or 32-bit float)
    channel : which channel to use (0 = left)

    Returns
    -------
    freqs, magnitude_db, sample_rate
    """
    sr, data = wavfile.read(path)

    # Normalise to float32 in [-1, 1]
    data = _norm_audio(data)

    # Select channel
    if data.ndim == 2:
        data = data[:, channel % data.shape[1]]

    freqs, psd = welch(data, fs=sr, nperseg=4096, noverlap=2048,
                       window='hann', scaling='density')

    # Convert PSD to dB (add tiny floor to avoid log(0))
    magnitude_db = 10.0 * np.log10(psd + 1e-12)

    return freqs, magnitude_db, sr


def load_impulse_response(path: str, channel: int = 0) -> FreqResponse:
    """
    Treat the WAV file as an impulse response (e.g. from REW or similar).
    Computes the magnitude spectrum via FFT with a Hann window.

    The IR is circularly shifted to put its peak at the centre of the buffer
    before windowing. This is not cosmetic: a Hann window is 0 at index 0 and
    1 at the centre, so windowing an IR *in place* attenuates it by however
    far its peak happens to sit from the file's midpoint. An exported IR
    normally has its peak at or near the start of the file, which is exactly
    where the window is zero -- the direct sound gets multiplied away and the
    resulting "response" is mostly the window's own shape. Measured against a
    known synthetic room, the un-shifted version was wrong by up to 13.7 dB
    (worst at peak-at-sample-0, tapering to ~0 only when the peak already
    happened to land mid-file); with the shift it is within 0.2 dB regardless
    of where the peak sits. A circular shift changes only the phase of the
    spectrum, never ``abs()`` of it, so this cannot distort the magnitude
    result it is protecting.

    It does *not* time-gate the IR: late reflections and the recording's
    noise floor are all still included. On a measurement with a poor noise
    floor (~-40 dB) that alone costs a couple of dB of accuracy.
    """
    sr, data = wavfile.read(path)
    data = _norm_audio(data)
    if data.ndim == 2:
        data = data[:, channel % data.shape[1]]

    # Centre the peak, then window the IR to reduce spectral leakage.
    if data.size:
        peak_index = int(np.argmax(np.abs(data)))
        data = np.roll(data, data.size // 2 - peak_index)
    win = get_window('hann', len(data))
    windowed = data * win

    spectrum = np.fft.rfft(windowed)
    freqs    = np.fft.rfftfreq(len(windowed), d=1.0 / sr)

    magnitude_db = 20.0 * np.log10(np.abs(spectrum) + 1e-12)

    return freqs, magnitude_db, sr


def smooth_octave(freqs: np.ndarray, magnitude_db: np.ndarray,
                  fraction: float = 1 / 3) -> Tuple[np.ndarray, np.ndarray]:
    """
    Apply fractional-octave smoothing to a magnitude response.

    For each bin, averages every bin whose frequency falls inside
    ``[fc * 2**(-fraction/2), fc * 2**(fraction/2)]``. Bins at or below 0 Hz
    (the DC bin) pass through untouched.

    Parameters
    ----------
    freqs        : frequency axis in Hz, ascending (must be > 0 for all
                   meaningful bins)
    magnitude_db : magnitude in dB
    fraction     : smoothing width in octaves (default: 1/3 octave)

    Returns
    -------
    freqs (the same object that was passed in), smoothed_db

    Performance
    -----------
    O(N log N). The previous implementation built a full-length boolean mask
    per bin, making it O(N^2) in both time and allocations. That was tolerable
    for a Welch PSD (nperseg=4096 -> ~2k bins) but not for the FFT-resolution
    paths: ``load_impulse_response`` returns one bin per two input samples, so
    a 5-second 48 kHz capture is ~120k bins (~3e10 element operations) and a
    30-second sweep is ~720k bins. `--ir` effectively hung. Windowed means now
    come from a prefix sum plus two binary searches per bin.
    """
    freqs_in = freqs
    freqs_arr = np.asarray(freqs, dtype=float)
    mag_arr = np.asarray(magnitude_db)

    smoothed = np.empty_like(mag_arr)
    if mag_arr.size == 0:
        return freqs_in, smoothed

    # searchsorted needs an ascending axis. Measured axes (welch, rfftfreq)
    # and synthetic log grids all are; sort defensively rather than silently
    # producing nonsense if a caller ever passes something else.
    order = None
    if np.any(np.diff(freqs_arr) < 0):
        order = np.argsort(freqs_arr, kind="stable")
        freqs_sorted = freqs_arr[order]
        mag_sorted = mag_arr[order]
    else:
        freqs_sorted = freqs_arr
        mag_sorted = mag_arr

    f_lo = freqs_sorted * 2.0 ** (-fraction / 2.0)
    f_hi = freqs_sorted * 2.0 ** ( fraction / 2.0)

    # Half-open window [lo, hi), matching the old mask exactly:
    # 'left' gives the first index with freq >= f_lo, 'right' the first with
    # freq > f_hi.
    lo = np.searchsorted(freqs_sorted, f_lo, side="left")
    hi = np.searchsorted(freqs_sorted, f_hi, side="right")

    csum = np.concatenate(([0.0], np.cumsum(mag_sorted, dtype=np.float64)))
    counts = hi - lo
    sums = csum[hi] - csum[lo]

    result = np.array(mag_sorted, dtype=np.float64, copy=True)
    valid = (freqs_sorted > 0) & (counts > 0)
    result[valid] = sums[valid] / counts[valid]

    if order is None:
        smoothed[:] = result
    else:
        smoothed[order] = result
    return freqs_in, smoothed


# ── Helpers ───────────────────────────────────────────────────────────────────

def _norm_audio(data: np.ndarray) -> np.ndarray:
    """Normalise integer PCM to float32 in [-1, 1]."""
    if data.dtype == np.int16:
        return data.astype(np.float32) / 32768.0
    if data.dtype == np.int32:
        return data.astype(np.float32) / 2_147_483_648.0
    if data.dtype == np.uint8:
        return (data.astype(np.float32) - 128.0) / 128.0
    # Already float
    return data.astype(np.float32)
