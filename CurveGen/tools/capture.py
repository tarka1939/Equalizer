"""Play a log sweep, record it, deconvolve to an impulse response."""
import sys, numpy as np, sounddevice as sd, scipy.io.wavfile as wav
from sweep import make_sweep, inverse_filter, deconvolve

SR, DUR, F1, F2 = 48000, 6.0, 20.0, 20000.0
OUT_DEV, IN_DEV = 13, 15          # set from sounddevice.query_devices()

def measure(out_wav, amp=0.10, pre=0.5, post=1.0):
    x = make_sweep(F1, F2, DUR, SR)
    sig = np.concatenate([np.zeros(int(pre*SR)), x*amp, np.zeros(int(post*SR))])
    play = np.column_stack([sig, sig]).astype(np.float32)

    rec = sd.playrec(play, samplerate=SR, channels=1,
                     device=(IN_DEV, OUT_DEV), dtype='float32', blocking=True)
    r = rec[:, 0].astype(np.float64)

    peak = float(np.max(np.abs(r)))
    noise = float(np.sqrt(np.mean(r[:int(pre*SR*0.8)]**2))) + 1e-12
    rms   = float(np.sqrt(np.mean(r[int(pre*SR):int((pre+DUR)*SR)]**2)))

    ir, _ = deconvolve(r, inverse_filter(x, F1, F2, DUR, SR), SR)
    wav.write(out_wav, SR, ir.astype(np.float32))
    print(f"{out_wav}: peak {20*np.log10(peak+1e-12):+.1f} dBFS, "
          f"SNR {20*np.log10(rms/noise):.1f} dB"
          + ("   *** CLIPPING ***" if peak >= 0.999 else ""))
    return ir

if __name__ == "__main__":
    measure(sys.argv[1], amp=float(sys.argv[2]) if len(sys.argv) > 2 else 0.10)
