"""
cli.py — Command-line interface for eq-curvegen.

Commands
--------
  measure     Analyse a WAV file and generate a room-correction preset
  eqapo       Analyse a WAV file and write an Equalizer APO config (offline,
              real-world validation -- see eqapo_export.py)
  visualize   Build a 4-stage FFT+CPB validation report (see visualize.py)
  plot        Display the frequency response and correction curve (requires matplotlib)
  send        Send a preset to the running eq-daemon via IPC

Examples
--------
  eq-curvegen measure    --input room.wav --output preset.json
  eq-curvegen measure    --input ir.wav --ir --harman --output preset.json
  eq-curvegen eqapo      --input room.wav --output config.txt --harman
  eq-curvegen eqapo      --input ir.wav --ir --mode parametric --filters 8 --output config.txt
  eq-curvegen eqapo      --channel-input L=left.wav --channel-input R=right.wav --ir --output config.txt
  eq-curvegen eqapo      --input stereo_ir.wav --ir --channels L,R --output config.txt
  eq-curvegen visualize  --input room_before.wav --output report.png
  eq-curvegen visualize  --input room_before.wav --recorded-output room_after.wav --output report.png
  eq-curvegen plot       --input room.wav
  eq-curvegen send       --preset preset.json
"""
from __future__ import annotations

import argparse
import json
import socket
import sys
from pathlib import Path

from curvegen import (measurement, flatten, export, visualize, eqapo_export,
                      parametric, channels)


# ── Shared analysis pipeline (measure / eqapo) ────────────────────────────────

def _load(args: argparse.Namespace, path: str, channel: int):
    """Load one channel of one measurement file and smooth it.

    Shared by every analysis path so the loading and smoothing steps cannot
    drift between single-channel and per-channel runs.
    """
    print(f"[curvegen] Loading {path} (channel {channel}) …")
    if args.ir:
        freqs, mag_db, sr = measurement.load_impulse_response(path, channel)
    else:
        freqs, mag_db, sr = measurement.load_wav(path, channel)

    print(f"[curvegen] Sample rate: {sr} Hz, {len(freqs)} frequency bins")
    freqs, mag_db = measurement.smooth_octave(freqs, mag_db, fraction=1 / 3)
    return freqs, mag_db, sr


def _solve_graphic(args: argparse.Namespace, freqs, mag_db, sr):
    """Fixed 10-band solve.

    `q` and `sr` must be the ones the playback chain will actually use. They
    feed the auto-preamp headroom calculation, and since the cascade solver
    landed they also determine the band gains themselves: Q sets how far each
    filter's skirt reaches into its neighbours, which is exactly what the
    solver is accounting for.
    """
    gains_db, preamp_db = flatten.compute_correction(
        freqs, mag_db,
        max_gain_db=args.max_gain,
        use_harman_target=args.harman,
        harman_blend=args.harman_blend,
        auto_preamp=not args.no_preamp,
        q=getattr(args, "q", 1.0),
        sample_rate=float(sr) if sr else flatten.DEFAULT_SAMPLE_RATE,
    )
    return flatten.DEFAULT_BAND_HZ, gains_db, preamp_db


def _solve_parametric(args: argparse.Namespace, freqs, mag_db, sr):
    """Free (Fc, Q, gain) solve."""
    return parametric.compute_parametric_correction(
        freqs, mag_db,
        n_filters=args.filters,
        q_range=tuple(args.q_range),
        freq_range=tuple(args.freq_range),
        max_gain_db=args.max_gain,
        use_harman_target=args.harman,
        harman_blend=args.harman_blend,
        auto_preamp=not args.no_preamp,
        sample_rate=float(sr) if sr else parametric.DEFAULT_SAMPLE_RATE,
    )


def _analyse(args: argparse.Namespace) -> tuple[list[float], "list[float]", float]:
    """
    Run the measurement + correction pipeline shared by `measure` and
    `eqapo`. Both commands must derive the curve from *exactly* the same
    code path -- the whole point of `eqapo` is to validate this pipeline's
    real-world behaviour, so it must not be a forked reimplementation that
    could silently drift from what `measure` actually produces.
    """
    freqs, mag_db, sr = _load(args, args.input, args.channel)
    band_hz, gains_db, preamp_db = _solve_graphic(args, freqs, mag_db, sr)
    print("\n Band (Hz)  Correction (dB)")
    print(" ─────────  ───────────────")
    for hz, g in zip(band_hz, gains_db):
        print(f"  {hz:>6.0f}    {g:+.2f}")
    print(f"\n Preamp:    {preamp_db:+.2f} dB\n")

    return band_hz, gains_db, preamp_db


# ── Subcommand: measure ───────────────────────────────────────────────────────

def cmd_measure(args: argparse.Namespace) -> int:
    band_hz, gains_db, preamp_db = _analyse(args)

    # Write preset
    name = args.name or Path(args.input).stem + "_correction"
    export.write_preset(args.output, name, gains_db, preamp_db, band_hz=band_hz)
    return 0


def _analyse_parametric(args: argparse.Namespace) -> "parametric.ParametricCurve":
    """
    Measurement + correction pipeline for `--mode parametric`.

    Deliberately parallel to `_analyse()` and identical to it up to the
    correction step, but it cannot share the whole thing: `_analyse()` returns
    per-band gains against a fixed grid, and the entire point here is that the
    grid is not fixed.
    """
    freqs, mag_db, sr = _load(args, args.input, args.channel)
    curve = _solve_parametric(args, freqs, mag_db, sr)

    print("\n  Filter   Fc (Hz)        Q   Gain (dB)")
    print("  ──────   ───────   ──────   ─────────")
    for i, (fc, q, g) in enumerate(zip(curve.fc_hz, curve.q, curve.gains_db), start=1):
        print(f"  {i:>6}   {fc:>7.1f}   {q:>6.2f}   {g:>+9.2f}")
    print(f"\n Preamp:    {curve.preamp_db:+.2f} dB")
    print(f" Filters:   {len(curve)} of {args.filters} requested\n")

    return curve


def _channel_sources(args: argparse.Namespace) -> "list[tuple[str, int, str, int]]":
    """Resolve the requested channels to (label, index, file path, file channel).

    Two ways to say it, because two workflows are natural:

    - `--channel-input L=left.wav --channel-input R=right.wav`: one file per
      speaker. This is what the capture tooling produces, since measuring a
      speaker means playing the sweep through that speaker alone.
    - `--input multichannel.wav --channels L,R`: several channels of one file,
      for a multichannel impulse-response export from something like REW.
    """
    if args.channel_input:
        sources = []
        for i, spec in enumerate(args.channel_input):
            if "=" not in spec:
                raise ValueError(
                    f"--channel-input expects LABEL=PATH, got {spec!r} "
                    "(e.g. --channel-input L=left.wav)")
            label, path = spec.split("=", 1)
            label = label.strip()
            if not label or not path.strip():
                raise ValueError(f"--channel-input expects LABEL=PATH, got {spec!r}")
            upper = label.upper()
            if not (upper in channels.KNOWN_ACRONYMS or label.isdigit()):
                raise ValueError(
                    f"channel label {label!r} is not an Equalizer APO position "
                    f"(expected one of {', '.join(channels.KNOWN_ACRONYMS)}, "
                    "or a 1-based number)")
            # Each file is measured on its own, so its channel 0 is the signal
            # unless the caller says otherwise with --channel.
            sources.append((upper if not label.isdigit() else label,
                            i, path.strip(), args.channel))
        return sources

    n = measurement.channel_count(args.input)
    indices = channels.parse_channel_spec(args.channels, n)
    return [(channels.label_for(i, n), i, args.input, i) for i in indices]


def _analyse_channels(args: argparse.Namespace) -> "channels.MultiChannelCurve":
    """Per-channel measurement + correction."""
    sources = _channel_sources(args)
    if not sources:
        raise ValueError("no channels selected")

    loaded = []
    for label, index, path, file_channel in sources:
        print(f"\n[curvegen] ── channel {label} ──")
        freqs, mag_db, sr = _load(args, path, file_channel)
        loaded.append((label, index, freqs, mag_db, sr))

    # Level matching, if asked for. Computed across every channel first, since
    # it is relative by definition. See channels.level_offsets_db for why this
    # is a gain rather than something the filter solver could express.
    if args.match_channels:
        offsets = channels.level_offsets_db([(f, m) for _, _, f, m, _ in loaded])
        levels = channels.channel_levels_db([(f, m) for _, _, f, m, _ in loaded])
        print("\n[curvegen] Channel level matching (relative, uncalibrated scale):")
        for (label, *_), lvl, off in zip(loaded, levels, offsets):
            print(f"             {label:>4}: {lvl:+7.2f} dB  ->  trim {off:+.2f} dB")
    else:
        offsets = [0.0] * len(loaded)

    curves = []
    n_channels = max((idx for _, idx, *_ in loaded), default=0) + 1
    if not args.channel_input:
        n_channels = measurement.channel_count(args.input)

    for (label, index, freqs, mag_db, sr), offset in zip(loaded, offsets):
        print(f"\n[curvegen] ── solving {label} ──")
        if args.mode == "parametric":
            pc = _solve_parametric(args, freqs, mag_db, sr)
            print("\n  Filter   Fc (Hz)        Q   Gain (dB)")
            print("  ──────   ───────   ──────   ─────────")
            for i, (fc, q, g) in enumerate(zip(pc.fc_hz, pc.q, pc.gains_db), start=1):
                print(f"  {i:>6}   {fc:>7.1f}   {q:>6.2f}   {g:>+9.2f}")
            print(f"\n Preamp:    {pc.preamp_db:+.2f} dB")
            print(f" Filters:   {len(pc)} of {args.filters} requested\n")
            curves.append(channels.ChannelCurve(
                label=label, index=index, band_hz=pc.fc_hz, gains_db=pc.gains_db,
                q=pc.q, preamp_db=pc.preamp_db, level_offset_db=offset))
        else:
            band_hz, gains_db, preamp_db = _solve_graphic(args, freqs, mag_db, sr)
            print("\n Band (Hz)  Correction (dB)")
            print(" ─────────  ───────────────")
            for hz, g in zip(band_hz, gains_db):
                print(f"  {hz:>6.0f}    {g:+.2f}")
            print(f"\n Preamp:    {preamp_db:+.2f} dB\n")
            curves.append(channels.ChannelCurve(
                label=label, index=index, band_hz=band_hz, gains_db=gains_db,
                q=args.q, preamp_db=preamp_db, level_offset_db=offset))

    curve = channels.MultiChannelCurve(curves=curves, n_channels=n_channels)
    print(f"[curvegen] Shared preamp: {curve.preamp_db:+.2f} dB "
          f"(the worst case across {len(curve)} channel(s); one preamp keeps "
          f"their relative levels intact)\n")
    return curve


# ── Subcommand: eqapo ──────────────────────────────────────────────────────────

def cmd_eqapo(args: argparse.Namespace) -> int:
    """
    Offline validation path: run the exact same measurement + correction
    pipeline as `measure`, but write an Equalizer APO config file instead of
    a JSON preset. See curvegen/eqapo_export.py for why this exists.
    """
    source = args.input or ", ".join(
        spec.split("=", 1)[-1] for spec in (args.channel_input or []))
    header = (
        f"Generated by eq-curvegen (offline Equalizer APO export) from {source}\n"
        "For OFFLINE validation of the curve-generation algorithm via Equalizer APO\n"
        "(https://equalizerapo.com) -- not part of this project's own daemon/APO.\n"
        "Load into Equalizer APO with an `Include:` line in config.txt, or paste\n"
        "directly into config.txt."
    )

    if args.channel_input and args.input:
        print("[curvegen] Use either --input or --channel-input, not both: "
              "--channel-input already names a file per channel.", file=sys.stderr)
        return 2
    if args.channels and not args.input:
        print("[curvegen] --channels selects channels of --input, so --input is "
              "required with it. To use one file per channel instead, pass "
              "--channel-input L=left.wav --channel-input R=right.wav.", file=sys.stderr)
        return 2
    if not args.input and not args.channel_input:
        print("[curvegen] --input is required (or --channel-input LABEL=PATH, "
              "repeated once per channel).", file=sys.stderr)
        return 2

    if args.channels or args.channel_input:
        try:
            multi = _analyse_channels(args)
        except ValueError as e:
            print(f"[curvegen] {e}", file=sys.stderr)
            return 2
        comment = (
            header
            + f"\n\nPer-channel correction: {', '.join(multi.labels)}."
            + "\nThe leading Preamp applies to every channel; each Channel block holds"
            + "\nthat speaker's own filters. The trailing 'Channel: all' resets the"
            + "\nselection so anything you Include after this file is unaffected."
            + ("\nChannel levels were matched (--match-channels)."
               if args.match_channels else "")
        )
        eqapo_export.write_multichannel_eqapo_config(args.output, multi, comment=comment)
        return 0

    if args.mode == "parametric":
        curve = _analyse_parametric(args)
        if len(curve) == 0:
            print("[curvegen] The solver placed no filters; nothing to write.", file=sys.stderr)
            return 1
        comment = (
            header
            + f"\n\nParametric mode: {len(curve)} filters with solver-chosen Fc and Q."
            + "\nIndividual gains are not meaningful on their own -- judge the curve as a whole."
        )
        eqapo_export.write_eqapo_config(
            args.output, list(curve.gains_db), band_hz=list(curve.fc_hz),
            preamp_db=curve.preamp_db, q=list(curve.q), comment=comment,
        )
        return 0

    band_hz, gains_db, preamp_db = _analyse(args)
    eqapo_export.write_eqapo_config(
        args.output, gains_db, band_hz=band_hz, preamp_db=preamp_db,
        q=args.q, comment=header,
    )
    return 0


# ── Subcommand: visualize ─────────────────────────────────────────────────────

def cmd_visualize(args: argparse.Namespace) -> int:
    """
    Build the 4-stage FFT+CPB validation report:
      1. Recorded input   -- args.input, as measured
      2. Curve generated   -- the continuous EQ response flatten.py computed
      3. Expected output   -- stage 1 + stage 2, computed mathematically
      4. Recorded output   -- args.recorded_output, if supplied (optional)
    See curvegen/visualize.py for the full explanation of each stage and of
    what "FFT" vs "CPB" mean here.
    """
    try:
        import matplotlib  # noqa: F401 -- import check only; visualize.plot_report does the real import
    except ImportError:
        print("matplotlib is required for `visualize`. Install with: pip install matplotlib")
        return 1

    print(f"[curvegen] Recorded input:  {args.input}")
    if args.recorded_output:
        print(f"[curvegen] Recorded output: {args.recorded_output}")
    else:
        print("[curvegen] Recorded output: (none supplied -- that panel will be marked unavailable)")

    report = visualize.build_report(
        recorded_input_path=args.input,
        recorded_output_path=args.recorded_output,
        input_format=args.input_format,
        output_format=args.output_format,
        input_channel=args.channel,
        output_channel=args.output_channel,
        ir=args.ir,
        harman=args.harman,
        max_gain_db=args.max_gain,
        q=args.q,
        cpb_fraction=args.cpb_fraction,
    )

    print("\n Band (Hz)  Requested gain (dB)")
    print(" ─────────  ───────────────────")
    for hz, g in zip(report.band_hz, report.gains_db):
        print(f"  {hz:>6.0f}    {g:+.2f}")
    print(f"\n Preamp:    {report.preamp_db:+.2f} dB\n")

    visualize.plot_report(report, args.output, cpb_fraction=args.cpb_fraction)
    print(f"[curvegen] Report saved to {args.output}")
    return 0


# ── Subcommand: plot ──────────────────────────────────────────────────────────

def cmd_plot(args: argparse.Namespace) -> int:
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib is required for --plot. Install with: pip install matplotlib")
        return 1

    if args.ir:
        freqs, mag_db, sr = measurement.load_impulse_response(args.input, args.channel)
    else:
        freqs, mag_db, sr = measurement.load_wav(args.input, args.channel)

    freqs_s, mag_s = measurement.smooth_octave(freqs, mag_db)
    gains_db, _ = flatten.compute_correction(freqs_s, mag_s)

    band_hz = flatten.DEFAULT_BAND_HZ
    import numpy as np
    log_bands = np.log10(band_hz)

    fig, axes = plt.subplots(2, 1, figsize=(10, 7), sharex=False)
    axes[0].semilogx(freqs[freqs > 20], mag_db[freqs > 20],  alpha=0.4, label="Raw")
    axes[0].semilogx(freqs_s[freqs_s > 20], mag_s[freqs_s > 20], label="Smoothed (1/3 oct)")
    axes[0].set_title("Measured Frequency Response")
    axes[0].set_xlabel("Frequency (Hz)")
    axes[0].set_ylabel("Magnitude (dB)")
    axes[0].legend()
    axes[0].grid(True, which='both', linestyle='--', alpha=0.5)

    axes[1].bar(range(len(band_hz)), gains_db, tick_label=[str(int(f)) for f in band_hz])
    axes[1].axhline(0, color='k', linewidth=0.8)
    axes[1].set_title("Correction EQ Gains")
    axes[1].set_xlabel("Band centre (Hz)")
    axes[1].set_ylabel("Gain (dB)")
    axes[1].grid(True, axis='y', linestyle='--', alpha=0.5)

    plt.tight_layout()
    plt.show()
    return 0


# ── Subcommand: send ──────────────────────────────────────────────────────────

def cmd_send(args: argparse.Namespace) -> int:
    preset = export.read_preset(args.preset)
    gains, preamp = export.preset_to_gains(preset)

    sock_path = "/tmp/eq-daemon.sock"
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.connect(sock_path)

            # Set preamp
            msg = json.dumps({"cmd": "set_preamp", "gain_db": preamp}) + "\n"
            s.sendall(msg.encode())
            resp = s.recv(256).decode().strip()
            print(f"[IPC] set_preamp → {resp}")

            # Set bands
            msg = json.dumps({"cmd": "set_bands", "gains_db": gains}) + "\n"
            s.sendall(msg.encode())
            resp = s.recv(256).decode().strip()
            print(f"[IPC] set_bands  → {resp}")

    except FileNotFoundError:
        print(f"[ERROR] Daemon socket not found at {sock_path}. Is eq-daemon running?")
        return 1
    except ConnectionRefusedError:
        print("[ERROR] Daemon is not accepting connections.")
        return 1

    return 0


# ── Entry point ───────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        prog="eq-curvegen",
        description="Acoustic room-correction EQ curve generator",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    # measure
    p_meas = sub.add_parser("measure", help="Analyse a WAV file and generate a preset")
    p_meas.add_argument("--input",    required=True,  help="Input WAV file")
    p_meas.add_argument("--output",   required=True,  help="Output preset JSON file")
    p_meas.add_argument("--ir",       action="store_true", help="Treat input as impulse response")
    p_meas.add_argument("--harman",   action="store_true", help="Blend toward Harman 2018 target")
    p_meas.add_argument("--harman-blend", type=float, default=0.5,
                        help="Harman blend amount, 0 = flat target, 1 = full Harman (default: 0.5). "
                             "Only meaningful with --harman.")
    p_meas.add_argument("--channel",  type=int, default=0, help="Audio channel to use (default: 0)")
    p_meas.add_argument("--max-gain", type=float, default=12.0, help="Max gain per band in dB")
    p_meas.add_argument("--no-preamp", action="store_true", help="Disable auto preamp headroom")
    p_meas.add_argument("--q",        type=float, default=1.0,
                        help="Q the playback chain will use; affects both the solved "
                             "band gains and the auto-preamp headroom, so it must "
                             "match what actually gets applied (default: 1.0)")
    p_meas.add_argument("--name",     default="", help="Preset name (default: input filename)")

    # eqapo
    p_eqapo = sub.add_parser(
        "eqapo",
        help="Analyse a WAV file and write an Equalizer APO config (offline real-world validation)",
    )
    # Not `required=True`: --channel-input supplies its own files, one per
    # channel, and then there is no single --input to name. cmd_eqapo checks
    # that exactly one of the two was given, which lets the error say what to
    # do instead of argparse's bare "--input is required".
    p_eqapo.add_argument("--input",    default=None,
                          help="Input WAV file. Required unless --channel-input is used.")
    p_eqapo.add_argument("--output",   required=True,  help="Output Equalizer APO config (.txt) file")
    p_eqapo.add_argument("--ir",       action="store_true", help="Treat input as impulse response")
    p_eqapo.add_argument("--harman",   action="store_true", help="Blend toward Harman 2018 target")
    p_eqapo.add_argument("--harman-blend", type=float, default=0.5,
                          help="Harman blend amount, 0 = flat target, 1 = full Harman (default: 0.5). "
                               "Only meaningful with --harman.")
    p_eqapo.add_argument("--channel",  type=int, default=0, help="Audio channel to use (default: 0)")
    p_eqapo.add_argument("--max-gain", type=float, default=12.0, help="Max gain per band in dB")
    p_eqapo.add_argument("--no-preamp", action="store_true", help="Disable auto preamp headroom")
    p_eqapo.add_argument("--q",        type=float, default=eqapo_export.DEFAULT_Q,
                          help=f"Shared Q for every band in graphic mode (default: "
                               f"{eqapo_export.DEFAULT_Q}). Ignored by --mode parametric, "
                               f"which chooses a Q per filter.")
    p_eqapo.add_argument("--channels", default=None, metavar="SPEC",
                          help="Correct several channels of --input separately, e.g. "
                               "'L,R' or 'all'. Positions are Equalizer APO acronyms "
                               "(L R C LFE RL RR RC SL SR) or 1-based numbers. Emits a "
                               "Channel: block per speaker.")
    p_eqapo.add_argument("--channel-input", action="append", default=None, metavar="LABEL=PATH",
                          help="One measurement file per channel, repeatable, e.g. "
                               "--channel-input L=left.wav --channel-input R=right.wav. "
                               "This is the usual shape for real measurements, since a "
                               "speaker is measured by playing the sweep through it alone.")
    p_eqapo.add_argument("--match-channels", action="store_true",
                          help="Also correct level imbalance between channels, by "
                               "trimming louder channels down to the quietest. Off by "
                               "default: with a single microphone a measured imbalance "
                               "is often the mic's position rather than the speakers, "
                               "and matching it would move the stereo image.")
    p_eqapo.add_argument("--mode", choices=("graphic", "parametric"), default="graphic",
                          help="graphic (default): gains only, on the fixed 10-band ISO grid "
                               "at a shared Q -- what DSP::Equalizer10Band can apply. "
                               "parametric: the solver also chooses each filter's centre "
                               "frequency and Q. Parametric curves can only be applied by "
                               "Equalizer APO -- the JSON preset schema and this project's "
                               "own DSP are both fixed at 10 bands.")
    p_eqapo.add_argument("--filters", type=int, default=parametric.DEFAULT_FILTERS,
                          help=f"Maximum filters to fit in parametric mode (default: "
                               f"{parametric.DEFAULT_FILTERS}). Fewer are emitted when extra "
                               f"filters would not measurably improve the fit.")
    p_eqapo.add_argument("--q-range", type=float, nargs=2, metavar=("MIN", "MAX"),
                          default=list(parametric.DEFAULT_Q_RANGE),
                          help=f"Q bounds for parametric mode (default: "
                               f"{parametric.DEFAULT_Q_RANGE[0]:g} {parametric.DEFAULT_Q_RANGE[1]:g})")
    p_eqapo.add_argument("--freq-range", type=float, nargs=2, metavar=("MIN", "MAX"),
                          default=list(parametric.DEFAULT_FREQ_RANGE),
                          help=f"Centre-frequency bounds in Hz for parametric mode (default: "
                               f"{parametric.DEFAULT_FREQ_RANGE[0]:g} "
                               f"{parametric.DEFAULT_FREQ_RANGE[1]:g}). Also clamped to the "
                               f"measured range and to Nyquist.")

    # visualize
    p_vis = sub.add_parser(
        "visualize",
        help="Build a 4-stage FFT+CPB validation report (recorded input / curve / expected / recorded output)",
    )
    p_vis.add_argument("--input",           required=True, help="Recorded input measurement (stage 1)")
    p_vis.add_argument("--recorded-output", default=None,
                        help="Recorded output measurement (stage 4), taken after applying the "
                             "correction. Optional -- omit if you haven't re-measured yet.")
    p_vis.add_argument("--output",          required=True, help="Output report image (e.g. report.png)")
    p_vis.add_argument("--input-format",    default=None,
                        help="Loader format for --input (default: auto-detect from extension, "
                             "falling back to 'wav'). See curvegen/loaders.py.")
    p_vis.add_argument("--output-format",   default=None,
                        help="Loader format for --recorded-output (default: same auto-detection as --input-format)")
    p_vis.add_argument("--ir",              action="store_true",
                        help="Treat --input/--recorded-output as impulse responses rather than raw recordings")
    p_vis.add_argument("--channel",         type=int, default=0, help="Channel to use for --input (default: 0)")
    p_vis.add_argument("--output-channel",  type=int, default=0, help="Channel to use for --recorded-output (default: 0)")
    p_vis.add_argument("--harman",          action="store_true", help="Blend toward Harman 2018 target")
    p_vis.add_argument("--max-gain",        type=float, default=flatten.MAX_GAIN_DB, help="Max gain per band in dB")
    p_vis.add_argument("--q",               type=float, default=1.0, help="Shared Q factor for every band (default: 1.0)")
    p_vis.add_argument("--cpb-fraction",    type=float, default=visualize.CPB_FRACTION,
                        help=f"Fractional-octave width for the CPB view (default: {visualize.CPB_FRACTION:.4f} = 1/3 octave)")

    # plot
    p_plot = sub.add_parser("plot", help="Plot frequency response and correction curve")
    p_plot.add_argument("--input",   required=True, help="Input WAV file")
    p_plot.add_argument("--ir",      action="store_true")
    p_plot.add_argument("--channel", type=int, default=0)

    # send
    p_send = sub.add_parser("send", help="Send a preset JSON to the running daemon")
    p_send.add_argument("--preset", required=True, help="Preset JSON file")

    args = parser.parse_args()

    handlers = {
        "measure":   cmd_measure,
        "eqapo":     cmd_eqapo,
        "visualize": cmd_visualize,
        "plot":      cmd_plot,
        "send":      cmd_send,
    }
    sys.exit(handlers[args.command](args))


if __name__ == "__main__":
    main()
