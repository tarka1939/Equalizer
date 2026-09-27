"""Play a log sweep, record it, deconvolve to an impulse response.

Measures one speaker at a time. A room-correction curve is per-speaker, so the
sweep has to come out of one speaker alone -- playing it through both at once
measures their sum at the microphone, which is a different (and much less
useful) thing: the two arrivals comb-filter against each other at the mic and
the result describes neither speaker.

  python capture.py room.wav                      # all output channels (as before)
  python capture.py room.wav --amp 0.13           # pick the level
  python capture.py room.wav --channel L          # left speaker only
  python capture.py --channels L,R --prefix room  # both, in sequence

The last form writes room_L.wav and room_R.wav, which is exactly what
`eq-curvegen eqapo --channel-input L=room_L.wav --channel-input R=room_R.wav`
consumes.

Only the playback routing knows about channels. The sweep generation and
deconvolution in sweep.py are untouched and are the part that was validated
offline -- see README.md.
"""
import argparse
import sys

import numpy as np
import scipy.io.wavfile as wav
import sounddevice as sd

from sweep import make_sweep, inverse_filter, deconvolve

SR, DUR, F1, F2 = 48000, 6.0, 20.0, 20000.0
OUT_DEV, IN_DEV = 13, 15          # set from sounddevice.query_devices()
OUT_CHANNELS = 2                  # output channels the device expects

# Position acronym -> 0-based output channel, matching Equalizer APO's layout
# table (see curvegen/channels.py). Only the layouts a capture rig plausibly
# has are listed; use a 1-based number for anything else.
CHANNEL_INDEX = {"L": 0, "R": 1, "C": 2, "LFE": 3, "RL": 4, "RR": 5, "SL": 6, "SR": 7}


def resolve_channel(name, out_channels=OUT_CHANNELS):
    """'L' / 'R' / '1' / 'all' -> 0-based index, or None for every channel."""
    if name is None or str(name).lower() == "all":
        return None
    key = str(name).strip()
    idx = int(key) - 1 if key.isdigit() else CHANNEL_INDEX.get(key.upper())
    if idx is None:
        raise SystemExit(f"unknown channel {name!r} (use L, R, C, LFE, RL, RR, SL, SR, "
                         "a 1-based number, or 'all')")
    if not (0 <= idx < out_channels):
        raise SystemExit(f"channel {name} is out of range for {out_channels} output channels")
    return idx


def measure(out_wav, amp=0.10, pre=0.5, post=1.0, channel=None, out_channels=OUT_CHANNELS):
    """Sweep, record, deconvolve, write an IR. `channel` = None means all."""
    x = make_sweep(F1, F2, DUR, SR)
    sig = np.concatenate([np.zeros(int(pre * SR)), x * amp, np.zeros(int(post * SR))])

    idx = resolve_channel(channel, out_channels)
    play = np.zeros((len(sig), out_channels), dtype=np.float32)
    if idx is None:
        play[:] = sig[:, None]
    else:
        play[:, idx] = sig

    rec = sd.playrec(play, samplerate=SR, channels=1,
                     device=(IN_DEV, OUT_DEV), dtype='float32', blocking=True)
    r = rec[:, 0].astype(np.float64)

    peak = float(np.max(np.abs(r)))
    noise = float(np.sqrt(np.mean(r[:int(pre * SR * 0.8)] ** 2))) + 1e-12
    rms = float(np.sqrt(np.mean(r[int(pre * SR):int((pre + DUR) * SR)] ** 2)))

    ir, _ = deconvolve(r, inverse_filter(x, F1, F2, DUR, SR), SR)
    wav.write(out_wav, SR, ir.astype(np.float32))
    where = "all channels" if idx is None else f"channel {channel}"
    print(f"{out_wav} [{where}]: peak {20 * np.log10(peak + 1e-12):+.1f} dBFS, "
          f"SNR {20 * np.log10(rms / noise):.1f} dB"
          + ("   *** CLIPPING ***" if peak >= 0.999 else ""))
    return ir


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("output", nargs="?", help="Output IR WAV (single measurement)")
    p.add_argument("amp_positional", nargs="?", type=float, default=None,
                   help=argparse.SUPPRESS)   # back-compat: capture.py out.wav 0.10
    p.add_argument("--amp", type=float, default=None, help="Playback amplitude (default 0.10)")
    p.add_argument("--channel", default=None,
                   help="Output channel to play through (L, R, a 1-based number, or all)")
    p.add_argument("--channels", default=None,
                   help="Measure several channels in sequence, e.g. L,R. Needs --prefix.")
    p.add_argument("--prefix", default=None,
                   help="Filename prefix for --channels; writes <prefix>_<CH>.wav")
    p.add_argument("--out-channels", type=int, default=OUT_CHANNELS,
                   help=f"Output channels the device expects (default {OUT_CHANNELS})")
    args = p.parse_args(argv)

    amp = args.amp if args.amp is not None else (
        args.amp_positional if args.amp_positional is not None else 0.10)

    if args.channels:
        if not args.prefix:
            p.error("--channels needs --prefix (output names are <prefix>_<CH>.wav)")
        names = [t for t in args.channels.replace(",", " ").split() if t]
        for name in names:
            resolve_channel(name, args.out_channels)     # validate all before playing
        for name in names:
            out = f"{args.prefix}_{name.upper()}.wav"
            input(f"\nAbout to measure {name.upper()} -> {out}. Press Enter when ready…")
            measure(out, amp=amp, channel=name, out_channels=args.out_channels)
        return 0

    if not args.output:
        p.error("an output file is required (or use --channels with --prefix)")
    measure(args.output, amp=amp, channel=args.channel, out_channels=args.out_channels)
    return 0


if __name__ == "__main__":
    sys.exit(main())
