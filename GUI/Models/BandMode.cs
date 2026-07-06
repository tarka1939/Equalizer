namespace EqualizerGUI.Models;

/// <summary>EQ band density modes.</summary>
public enum BandMode
{
    Octave          = 0,   // ISO 1-octave:   10 bands
    TwoThirdsOctave = 1,   // ISO 2/3-octave: 15 bands
    ThirdOctave     = 2,   // ISO 1/3-octave: 31 bands
}

/// <summary>Display names and ISO centre-frequency tables for each <see cref="BandMode"/>.</summary>
public static class BandModeInfo
{
    public static string ToDisplayName(this BandMode mode) => mode switch
    {
        BandMode.Octave          => "1 Octave (10 bands)",
        BandMode.TwoThirdsOctave => "2/3 Octave (15 bands)",
        BandMode.ThirdOctave     => "1/3 Octave (31 bands)",
        _                        => mode.ToString(),
    };

    /// <summary>ISO centre frequencies for a given band mode.</summary>
    public static double[] GetCentres(this BandMode mode) => mode switch
    {
        BandMode.Octave          => Octave,
        BandMode.TwoThirdsOctave => TwoThirds,
        BandMode.ThirdOctave     => ThirdOctave,
        _                        => Octave,
    };

    // ── Frequency tables ──────────────────────────────────────────────────────

    public static readonly double[] Octave =
        { 31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

    public static readonly double[] TwoThirds =
        { 25, 40, 63, 100, 160, 250, 400, 630, 1000, 1600, 2500, 4000, 6300, 10000, 16000 };

    public static readonly double[] ThirdOctave =
    {
        20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160,
        200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600,
        2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000,
    };

    /// <summary>The 10 standard octave positions used by the daemon (always).</summary>
    public static readonly double[] DaemonBands = Octave;
}

/// <summary>
/// Wraps a <see cref="BandMode"/> with a display-friendly <see cref="ToString"/>
/// so it can be used directly in Avalonia ComboBox without a converter.
/// </summary>
public sealed record BandModeOption(BandMode Mode)
{
    public override string ToString() => Mode.ToDisplayName();
}
