"""
measurement.py — Load and analyse audio measurements for room correction.

Supported inputs:
  - WAV file (mono or stereo; first channel used)
  - Swept-sine (logarithmic sweep) deconvolution for impulse response
  - Pink noise RTA (simpler, less accurate)

All functions return:
    freqs        : np.ndarray of shape (N,)  — frequency axis in Hz
    magnitude_db : np.ndarray of shape (N,)  — average magnitude in dB
    sample_rate  : int
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
    """
    sr, data = wavfile.read(path)
    data = _norm_audio(data)
    if data.ndim == 2:
        data = data[:, channel % data.shape[1]]

    # Window the IR to reduce spectral leakage
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

    Parameters
    ----------
    freqs        : frequency axis in Hz (must be > 0 for all meaningful bins)
    magnitude_db : magnitude in dB
    fraction     : smoothing width in octaves (default: 1/3 octave)

    Returns
    -------
    freqs (unchanged), smoothed_db
    """
    smoothed = np.empty_like(magnitude_db)
    for i, fc in enumerate(freqs):
        if fc <= 0:
            smoothed[i] = magnitude_db[i]
            continue
        f_lo = fc * 2 ** (-fraction / 2)
        f_hi = fc * 2 ** ( fraction / 2)
        mask = (freqs >= f_lo) & (freqs <= f_hi)
        smoothed[i] = np.mean(magnitude_db[mask]) if np.any(mask) else magnitude_db[i]
    return freqs, smoothed


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
