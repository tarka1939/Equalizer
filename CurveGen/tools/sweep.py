"""Exponential-sine-sweep generation and deconvolution (Farina method)."""
import numpy as np


def make_sweep(f1=20.0, f2=20000.0, dur=6.0, sr=48000, fade=0.05):
    """Farina exponential sine sweep, amplitude 1.0, with raised-cosine fades."""
    n = int(dur * sr)
    t = np.arange(n) / sr
    K = dur * 2 * np.pi * f1 / np.log(f2 / f1)
    L = np.log(f2 / f1) / dur
    x = np.sin(K * (np.exp(t * L) - 1.0))
    nf = max(1, int(fade * sr))
    w = 0.5 * (1 - np.cos(np.pi * np.arange(nf) / nf))
    x[:nf] *= w
    x[-nf:] *= w[::-1]
    return x.astype(np.float64)


def inverse_filter(x, f1=20.0, f2=20000.0, dur=6.0, sr=48000):
    """Time-reversed sweep with a -6 dB/octave envelope, so sweep * inverse -> delta."""
    n = len(x)
    t = np.arange(n) / sr
    L = np.log(f2 / f1) / dur
    return (x[::-1] * np.exp(-t * L)).astype(np.float64)


def deconvolve(recorded, inv, sr=48000, ir_len=16384):
    """Recover the impulse response, keeping ir_len samples around the peak."""
    n = 1
    while n < len(recorded) + len(inv):
        n *= 2
    full = np.fft.irfft(np.fft.rfft(recorded, n) * np.fft.rfft(inv, n), n)
    peak = int(np.argmax(np.abs(full)))
    pre = min(peak, 256)
    ir = full[peak - pre: peak - pre + ir_len]
    if len(ir) < ir_len:
        ir = np.pad(ir, (0, ir_len - len(ir)))
    m = np.max(np.abs(ir))
    return (ir / m if m > 0 else ir), peak
