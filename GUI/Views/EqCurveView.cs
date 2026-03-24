using System.Collections.ObjectModel;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using EqualizerGUI.ViewModels;

namespace EqualizerGUI.Views;

/// <summary>
/// Custom Avalonia control that draws the EQ response curve
/// as a smooth polyline over a log-frequency grid.
/// </summary>
public sealed class EqCurveView : Control
{
    // ── Avalonia dependency property ──────────────────────────────────────────
    public static readonly DirectProperty<EqCurveView, ObservableCollection<BandViewModel>?> BandsProperty =
        AvaloniaProperty.RegisterDirect<EqCurveView, ObservableCollection<BandViewModel>?>(
            nameof(Bands), o => o.Bands, (o, v) => o.Bands = v);

    private ObservableCollection<BandViewModel>? _bands;
    public ObservableCollection<BandViewModel>? Bands
    {
        get => _bands;
        set
        {
            if (_bands is not null)
                _bands.CollectionChanged -= OnBandsChanged;

            SetAndRaise(BandsProperty, ref _bands, value);

            if (_bands is not null)
            {
                _bands.CollectionChanged += OnBandsChanged;
                // Re-subscribe to each band's GainDb property.
                foreach (var b in _bands)
                    b.PropertyChanged += (_, _) => InvalidateVisual();
            }
            InvalidateVisual();
        }
    }

    private void OnBandsChanged(object? sender, System.Collections.Specialized.NotifyCollectionChangedEventArgs e)
        => InvalidateVisual();

    // ── Rendering ─────────────────────────────────────────────────────────────
    public override void Render(DrawingContext ctx)
    {
        var bounds = Bounds;
        double w = bounds.Width;
        double h = bounds.Height;

        if (w <= 0 || h <= 0 || _bands is null || _bands.Count == 0)
            return;

        const double minDb  = -13.0;
        const double maxDb  =  13.0;
        const double dbRange = maxDb - minDb;

        double DbToY(double db) => h - (db - minDb) / dbRange * h;
        double BandToX(int idx) => (idx + 0.5) / _bands.Count * w;

        // ── Grid ─────────────────────────────────────────────────────────────
        var gridPen = new Pen(new SolidColorBrush(Color.Parse("#222240")), 1);
        // 0 dB centre line
        var centrePen = new Pen(new SolidColorBrush(Color.Parse("#3A3A60")), 1,
            dashStyle: DashStyle.Dash);

        for (double db = -12; db <= 12; db += 3)
        {
            double y = DbToY(db);
            var pen = db == 0 ? centrePen : gridPen;
            ctx.DrawLine(pen, new Point(0, y), new Point(w, y));
        }

        // ── Curve ─────────────────────────────────────────────────────────────
        if (_bands.Count < 2) return;

        var points = new Point[_bands.Count];
        for (int i = 0; i < _bands.Count; i++)
            points[i] = new Point(BandToX(i), DbToY(_bands[i].GainDb));

        // Build a StreamGeometry for the filled area under the curve.
        var geom = new StreamGeometry();
        using (var ctx2 = geom.Open())
        {
            ctx2.BeginFigure(new Point(points[0].X, h), true);
            ctx2.LineTo(points[0]);
            for (int i = 1; i < points.Length; i++)
            {
                // Simple cubic Catmull-Rom-style smoothing.
                var p0 = i > 1               ? points[i - 2] : points[i - 1];
                var p1 = points[i - 1];
                var p2 = points[i];
                var p3 = i < points.Length - 1 ? points[i + 1] : points[i];

                var cp1 = new Point(p1.X + (p2.X - p0.X) / 6.0,
                                    p1.Y + (p2.Y - p0.Y) / 6.0);
                var cp2 = new Point(p2.X - (p3.X - p1.X) / 6.0,
                                    p2.Y - (p3.Y - p1.Y) / 6.0);
                ctx2.CubicBezierTo(cp1, cp2, p2);
            }
            ctx2.LineTo(new Point(points[^1].X, h));
            ctx2.EndFigure(true);
        }

        // Fill below curve
        var fillBrush = new LinearGradientBrush
        {
            StartPoint = new RelativePoint(0.5, 0, RelativeUnit.Relative),
            EndPoint   = new RelativePoint(0.5, 1, RelativeUnit.Relative),
            GradientStops = [
                new GradientStop(Color.FromArgb(90, 124, 92, 191), 0),
                new GradientStop(Color.FromArgb(10, 124, 92, 191), 1),
            ]
        };
        ctx.DrawGeometry(fillBrush, null, geom);

        // Stroke the curve
        var curvePen = new Pen(new SolidColorBrush(Color.Parse("#9B79F0")), 2.5);
        // Re-draw just the line (reuse points array).
        for (int i = 1; i < points.Length; i++)
            ctx.DrawLine(curvePen, points[i - 1], points[i]);

        // Band dots
        var dotBrush = new SolidColorBrush(Color.Parse("#C8B8FF"));
        foreach (var pt in points)
            ctx.DrawEllipse(dotBrush, null, pt, 4, 4);
    }
}
