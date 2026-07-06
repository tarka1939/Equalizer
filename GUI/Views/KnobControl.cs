using System;
using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;

namespace EqualizerGUI.Views;

/// <summary>
/// Rotary knob control. Drag upward to increase, downward to decrease.
/// Double-click resets to 0.
/// </summary>
public sealed class KnobControl : Control
{
    // ── Styled properties ─────────────────────────────────────────────────────

    public static readonly StyledProperty<double> ValueProperty =
        AvaloniaProperty.Register<KnobControl, double>(nameof(Value), 0.0);

    public static readonly StyledProperty<double> MinimumProperty =
        AvaloniaProperty.Register<KnobControl, double>(nameof(Minimum), -12.0);

    public static readonly StyledProperty<double> MaximumProperty =
        AvaloniaProperty.Register<KnobControl, double>(nameof(Maximum), 12.0);

    public static readonly StyledProperty<string> LabelProperty =
        AvaloniaProperty.Register<KnobControl, string>(nameof(Label), "");

    public double Value   { get => GetValue(ValueProperty);   set => SetValue(ValueProperty,   Clamp(value)); }
    public double Minimum { get => GetValue(MinimumProperty); set => SetValue(MinimumProperty, value); }
    public double Maximum { get => GetValue(MaximumProperty); set => SetValue(MaximumProperty, value); }
    public string Label   { get => GetValue(LabelProperty);   set => SetValue(LabelProperty,   value); }

    private double Clamp(double v) => Math.Clamp(v, Minimum, Maximum);

    // ── Constants ─────────────────────────────────────────────────────────────
    // All angles are degrees measured CW from 12 o'clock (top).
    private const double MinAngle      = -150.0;   // 7 o'clock
    private const double MaxAngle      =  150.0;   // 5 o'clock
    private const double PixelsPerUnit =  150.0;   // drag pixels per full range

    // ── Drag state ────────────────────────────────────────────────────────────
    private bool   _dragging;
    private double _dragStartY;
    private double _dragStartValue;
    private DateTime _lastPress;

    // ── Static ctor – wire up InvalidateVisual ────────────────────────────────
    static KnobControl()
    {
        ValueProperty  .Changed.AddClassHandler<KnobControl>((k, _) => k.InvalidateVisual());
        MinimumProperty.Changed.AddClassHandler<KnobControl>((k, _) => k.InvalidateVisual());
        MaximumProperty.Changed.AddClassHandler<KnobControl>((k, _) => k.InvalidateVisual());
        LabelProperty  .Changed.AddClassHandler<KnobControl>((k, _) => k.InvalidateVisual());
    }

    protected override Size MeasureOverride(Size _) => new(76, 96);

    // ── Rendering ─────────────────────────────────────────────────────────────
    public override void Render(DrawingContext ctx)
    {
        double w = Bounds.Width;
        double h = Bounds.Height;

        bool hasLabel = !string.IsNullOrEmpty(Label);
        double labelH = hasLabel ? 18 : 0;
        double knobR  = Math.Min(w / 2, (h - labelH) / 2) - 5;
        double cx = w / 2;
        double cy = knobR + 5;

        // 1. Outer ring
        ctx.DrawEllipse(
            new SolidColorBrush(Color.Parse("#1E1E32")),
            new Pen(new SolidColorBrush(Color.Parse("#38385A")), 1.5),
            new Point(cx, cy), knobR + 4, knobR + 4);

        // 2. Track arc (full range, dim)
        DrawArc(ctx, cx, cy, knobR, MinAngle, MaxAngle,
                new Pen(new SolidColorBrush(Color.Parse("#2C2C48")), 4));

        // 3. Value arc (from 0 dB → current value)
        double valAngle = ValueToAngle();
        if (Math.Abs(Value) > 0.05)
        {
            double arcA = Math.Min(valAngle, 0.0);
            double arcB = Math.Max(valAngle, 0.0);
            var color = Value > 0 ? Color.Parse("#9B79F0") : Color.Parse("#5BBCDE");
            DrawArc(ctx, cx, cy, knobR, arcA, arcB,
                    new Pen(new SolidColorBrush(color), 4));
        }

        // 4. Indicator line
        var tip   = AngleToPoint(cx, cy, knobR - 5, valAngle);
        var inner = AngleToPoint(cx, cy, knobR * 0.32, valAngle);
        ctx.DrawLine(new Pen(new SolidColorBrush(Color.Parse("#E0D0FF")), 2.5), inner, tip);

        // 5. Center dot
        ctx.DrawEllipse(new SolidColorBrush(Color.Parse("#6050A0")), null, new Point(cx, cy), 4, 4);

        // 6. Value text
        string valStr = Value switch { > 0.001 => $"+{Value:0.0}", < -0.001 => $"{Value:0.0}", _ => "0.0" };
        var    valFt  = new FormattedText(valStr, CultureInfo.InvariantCulture,
                            FlowDirection.LeftToRight, Typeface.Default, 10,
                            new SolidColorBrush(Color.Parse("#BBA8EE")));
        ctx.DrawText(valFt, new Point(cx - valFt.Width / 2, cy - valFt.Height / 2));

        // 7. Label
        if (hasLabel)
        {
            var lFt = new FormattedText(Label, CultureInfo.InvariantCulture,
                          FlowDirection.LeftToRight, Typeface.Default, 11,
                          new SolidColorBrush(Color.Parse("#9080B8")));
            ctx.DrawText(lFt, new Point(cx - lFt.Width / 2, cy + knobR + 8));
        }
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    private double ValueToAngle()
    {
        double t = (Maximum - Minimum) > 0
            ? (Value - Minimum) / (Maximum - Minimum)
            : 0.5;
        return MinAngle + Math.Clamp(t, 0, 1) * (MaxAngle - MinAngle);
    }

    private static Point AngleToPoint(double cx, double cy, double r, double angleDeg)
    {
        double rad = angleDeg * Math.PI / 180.0;
        return new Point(cx + r * Math.Sin(rad), cy - r * Math.Cos(rad));
    }

    private static void DrawArc(DrawingContext ctx, double cx, double cy, double r,
                                 double startDeg, double endDeg, IPen pen)
    {
        const int Steps = 36;
        double sweep = endDeg - startDeg;
        if (Math.Abs(sweep) < 0.01) return;
        for (int i = 0; i < Steps; i++)
        {
            double a1 = startDeg + sweep * i / Steps;
            double a2 = startDeg + sweep * (i + 1) / Steps;
            ctx.DrawLine(pen, AngleToPoint(cx, cy, r, a1), AngleToPoint(cx, cy, r, a2));
        }
    }

    // ── Pointer events ────────────────────────────────────────────────────────

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        e.Pointer.Capture(this);
        _dragging       = true;
        _dragStartY     = e.GetCurrentPoint(this).Position.Y;
        _dragStartValue = Value;

        // Double-click reset to 0
        if ((DateTime.Now - _lastPress).TotalMilliseconds < 300)
            Value = Clamp(0.0);
        _lastPress = DateTime.Now;

        e.Handled = true;
        base.OnPointerPressed(e);
    }

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        if (!_dragging) return;
        double range = Maximum - Minimum;
        double delta = (_dragStartY - e.GetCurrentPoint(this).Position.Y) * range / PixelsPerUnit;
        Value     = Clamp(_dragStartValue + delta);
        e.Handled = true;
        base.OnPointerMoved(e);
    }

    protected override void OnPointerReleased(PointerReleasedEventArgs e)
    {
        if (_dragging)
        {
            _dragging = false;
            e.Pointer.Capture(null);
            e.Handled = true;
        }
        base.OnPointerReleased(e);
    }
}
