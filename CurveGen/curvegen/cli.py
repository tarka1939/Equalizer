"""
cli.py — Command-line interface for eq-curvegen.

Commands
--------
  measure     Analyse a WAV file and generate a room-correction preset
  visualize   Build a 4-stage FFT+CPB validation report (see visualize.py)
  plot        Display the frequency response and correction curve (requires matplotlib)
  send        Send a preset to the running eq-daemon via IPC

Examples
--------
  eq-curvegen measure    --input room.wav --output preset.json
  eq-curvegen measure    --input ir.wav --ir --harman --output preset.json
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

from curvegen import measurement, flatten, export, visualize


# ── Subcommand: measure ───────────────────────────────────────────────────────

def cmd_measure(args: argparse.Namespace) -> int:
    print(f"[curvegen] Loading {args.input} …")
    if args.ir:
        freqs, mag_db, sr = measurement.load_impulse_response(args.input, args.channel)
    else:
        freqs, mag_db, sr = measurement.load_wav(args.input, args.channel)

    print(f"[curvegen] Sample rate: {sr} Hz, {len(freqs)} frequency bins")

    # Smooth
    freqs, mag_db = measurement.smooth_octave(freqs, mag_db, fraction=1 / 3)

    # Compute correction
    gains_db, preamp_db = flatten.compute_correction(
        freqs, mag_db,
        max_gain_db=args.max_gain,
        use_harman_target=args.harman,
        harman_blend=0.5,
        auto_preamp=not args.no_preamp,
    )

    # Report
    band_hz = flatten.DEFAULT_BAND_HZ
    print("\n Band (Hz)  Correction (dB)")
    print(" ─────────  ───────────────")
    for hz, g in zip(band_hz, gains_db):
        print(f"  {hz:>6.0f}    {g:+.2f}")
    print(f"\n Preamp:    {preamp_db:+.2f} dB\n")

    # Write preset
    name = args.name or Path(args.input).stem + "_correction"
    export.write_preset(args.output, name, gains_db, preamp_db)
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
    p_meas.add_argument("--channel",  type=int, default=0, help="Audio channel to use (default: 0)")
    p_meas.add_argument("--max-gain", type=float, default=12.0, help="Max gain per band in dB")
    p_meas.add_argument("--no-preamp", action="store_true", help="Disable auto preamp headroom")
    p_meas.add_argument("--name",     default="", help="Preset name (default: input filename)")

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
        "visualize": cmd_visualize,
        "plot":      cmd_plot,
        "send":      cmd_send,
    }
    sys.exit(handlers[args.command](args))


if __name__ == "__main__":
    main()
