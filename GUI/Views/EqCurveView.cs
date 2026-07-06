using System;
using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.ComponentModel;
using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using EqualizerGUI.ViewModels;

namespace EqualizerGUI.Views;

/// <summary>
/// Canvas that draws two independent EQ curves on a log-frequency grid:
///   • <see cref="UserBands"/> – purple – the user's manual EQ adjustments.
///   • <see cref="CalibrationBands"/> – teal – output of the calibration utility.
/// X axis is log-frequency 20 Hz → 20 kHz.
/// Y axis is ±13 dB (clamped display range).
/// </summary>
public sealed class EqCurveView : Control
{
    // ── Dependency properties ─────────────────────────────────────────────────

    public static readonly DirectProperty<EqCurveView, ObservableCollection<BandViewModel>?> UserBandsProperty =
        AvaloniaProperty.RegisterDirect<EqCurveView, ObservableCollection<BandViewModel>?>(
            nameof(UserBands), o => o.UserBands, (o, v) => o.UserBands = v);

    public static readonly DirectProperty<EqCurveView, ObservableCollection<BandViewModel>?> CalibrationBandsProperty =
        AvaloniaProperty.RegisterDirect<EqCurveView, ObservableCollection<BandViewModel>?>(
            nameof(CalibrationBands), o => o.CalibrationBands, (o, v) => o.CalibrationBands = v);

    private ObservableCollection<BandViewModel>? _userBands;
    private ObservableCollection<BandViewModel>? _calibrationBands;

    public ObservableCollection<BandViewModel>? UserBands
    {
        get => _userBands;
        set { Resubscribe(ref _userBands, value); SetAndRaise(UserBandsProperty, ref _userBands, value); }
    }

    public ObservableCollection<BandViewModel>? CalibrationBands
    {
        get => _calibrationBands;
        set { Resubscribe(ref _calibrationBands, value); SetAndRaise(CalibrationBandsProperty, ref _calibrationBands, value); }
    }

    // ── Subscription helpers ──────────────────────────────────────────────────

    private void Resubscribe(ref ObservableCollection<BandViewModel>? field,
                              ObservableCollection<BandViewModel>? newCol)
    {
        if (field is not null)
        {
            field.CollectionChanged -= OnCollectionChanged;
            foreach (var b in field) b.PropertyChanged -= OnBandChanged;
        }
        if (newCol is not null)
        {
            newCol.CollectionChanged += OnCollectionChanged;
            foreach (var b in newCol) b.PropertyChanged += OnBandChanged;
        }
        InvalidateVisual();
    }

    private void OnCollectionChanged(object? s, NotifyCollectionChangedEventArgs e)
    {
        if (e.OldItems is not null) foreach (BandViewModel b in e.OldItems) b.PropertyChanged -= OnBandChanged;
        if (e.NewItems is not null) foreach (BandViewModel b in e.NewItems) b.PropertyChanged += OnBandChanged;
        InvalidateVisual();
    }

    private void OnBandChanged(object? s, PropertyChangedEventArgs e) => InvalidateVisual();

    // ── Frequency / dB mapping ────────────────────────────────────────────────

    private const double MinHz  = 20.0;
    private const double MaxHz  = 20000.0;
    private const double MinDb  = -13.0;
    private const double MaxDb  =  13.0;
    private const double DbRange = MaxDb - MinDb;

    private static double HzToX(double hz, double w) =>
        (Math.Log10(Math.Clamp(hz, MinHz, MaxHz)) - Math.Log10(MinHz))
        / (Math.Log10(MaxHz) - Math.Log10(MinHz)) * w;

    private static double DbToY(double db, double h) =>
        h - (Math.Clamp(db, MinDb, MaxDb) - MinDb) / DbRange * h;

    // ── Render ────────────────────────────────────────────────────────────────

    public override void Render(DrawingContext ctx)
    {
        double w = Bounds.Width;
        double h = Bounds.Height;
        if (w <= 0 || h <= 0) return;

        DrawGrid(ctx, w, h);
        DrawCurve(ctx, _calibrationBands, w, h, teal: true);
        DrawCurve(ctx, _userBands, w, h, teal: false);
        DrawHzLabels(ctx, w, h);
        DrawLegend(ctx, w);
    }

    // ── Grid ──────────────────────────────────────────────────────────────────

    private static readonly (double db, bool bright)[] DbGridLines =
    {
        (-12, false), (-9, false), (-6, false), (-3, false),
        (0, true),
        (3, false), (6, false), (9, false), (12, false),
    };

    private static readonly double[] HzGridLines =
        { 31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

    private static readonly (string label, double hz)[] HzLabels =
    {
        ("31", 31.5), ("63", 63), ("125", 125), ("250", 250), ("500", 500),
        ("1k", 1000), ("2k", 2000), ("4k", 4000), ("8k", 8000), ("16k", 16000),
    };

    private static void DrawGrid(DrawingContext ctx, double w, double h)
    {
        var dimPen    = new Pen(new SolidColorBrush(Color.Parse("#1E1E38")), 1);
        var centrePen = new Pen(new SolidColorBrush(Color.Parse("#38385A")), 1.5);

        foreach (var (db, bright) in DbGridLines)
        {
            double y = DbToY(db, h);
            ctx.DrawLine(bright ? centrePen : dimPen, new Point(0, y), new Point(w, y));
        }
        foreach (var hz in HzGridLines)
        {
            double x = HzToX(hz, w);
            ctx.DrawLine(dimPen, new Point(x, 0), new Point(x, h));
        }
    }

    // ── Curve drawing ─────────────────────────────────────────────────────────

    private static void DrawCurve(DrawingContext ctx,
                                   ObservableCollection<BandViewModel>? bands,
                                   double w, double h, bool teal)
    {
        if (bands is null || bands.Count < 2) return;

        // Build screen-space points
        var pts = new Point[bands.Count];
        for (int i = 0; i < bands.Count; i++)
            pts[i] = new Point(HzToX(bands[i].Hz, w), DbToY(bands[i].GainDb, h));

        // Filled area under curve
        var fill = BuildFillGeometry(pts, h);
        var (r, g, b) = teal ? (0x40, 0xB8, 0xC0) : (0x80, 0x50, 0xCC);
        var fillBrush = new LinearGradientBrush
        {
            StartPoint = new RelativePoint(0.5, 0, RelativeUnit.Relative),
            EndPoint   = new RelativePoint(0.5, 1, RelativeUnit.Relative),
            GradientStops =
            [
                new GradientStop(Color.FromArgb(70,  (byte)r, (byte)g, (byte)b), 0),
                new GradientStop(Color.FromArgb(8,   (byte)r, (byte)g, (byte)b), 1),
            ],
        };
        ctx.DrawGeometry(fillBrush, null, fill);

        // Stroke
        var strokeColor = teal ? Color.Parse("#50D0D8") : Color.Parse("#9B79F0");
        var pen = new Pen(new SolidColorBrush(strokeColor), teal ? 1.8 : 2.5);
        for (int i = 1; i < pts.Length; i++)
            ctx.DrawLine(pen, pts[i - 1], pts[i]);

        // Band dots
        var dotBrush = new SolidColorBrush(strokeColor);
        double dotR = teal ? 2.5 : 3.5;
        foreach (var pt in pts)
            ctx.DrawEllipse(dotBrush, null, pt, dotR, dotR);
    }

    private static StreamGeometry BuildFillGeometry(Point[] pts, double h)
    {
        var geom = new StreamGeometry();
        using var sg = geom.Open();
        sg.BeginFigure(new Point(pts[0].X, h), true);
        sg.LineTo(pts[0]);
        for (int i = 1; i < pts.Length; i++)
        {
            var p0 = i > 1               ? pts[i - 2] : pts[i - 1];
            var p1 = pts[i - 1];
            var p2 = pts[i];
            var p3 = i < pts.Length - 1 ? pts[i + 1] : pts[i];
            var cp1 = new Point(p1.X + (p2.X - p0.X) / 6, p1.Y + (p2.Y - p0.Y) / 6);
            var cp2 = new Point(p2.X - (p3.X - p1.X) / 6, p2.Y - (p3.Y - p1.Y) / 6);
            sg.CubicBezierTo(cp1, cp2, p2);
        }
        sg.LineTo(new Point(pts[^1].X, h));
        sg.EndFigure(true);
        return geom;
    }

    // ── Hz axis labels ────────────────────────────────────────────────────────

    private static void DrawHzLabels(DrawingContext ctx, double w, double h)
    {
        var brush = new SolidColorBrush(Color.Parse("#50507A"));
        foreach (var (label, hz) in HzLabels)
        {
            var ft = new FormattedText(label, CultureInfo.InvariantCulture,
                         FlowDirection.LeftToRight, Typeface.Default, 10, brush);
            double x = HzToX(hz, w) - ft.Width / 2;
            ctx.DrawText(ft, new Point(x, h - ft.Height - 1));
        }
    }

    // ── Legend ────────────────────────────────────────────────────────────────

    private void DrawLegend(DrawingContext ctx, double w)
    {
        bool hasCal = _calibrationBands is { Count: > 0 };
        if (!hasCal) return;

        const double px = 10;
        const double py = 6;
        DrawLegendItem(ctx, px,            py, Color.Parse("#9B79F0"), "User EQ");
        DrawLegendItem(ctx, px + 90,       py, Color.Parse("#50D0D8"), "Calibration");
    }

    private static void DrawLegendItem(DrawingContext ctx, double x, double y,
                                        Color color, string label)
    {
        ctx.DrawLine(new Pen(new SolidColorBrush(color), 2.5),
                     new Point(x, y + 5), new Point(x + 16, y + 5));
        var ft = new FormattedText(label, CultureInfo.InvariantCulture,
                     FlowDirection.LeftToRight, Typeface.Default, 10,
                     new SolidColorBrush(Color.Parse("#8080A8")));
        ctx.DrawText(ft, new Point(x + 20, y));
    }
}
