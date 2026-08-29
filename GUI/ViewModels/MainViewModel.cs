using System.Collections.ObjectModel;
using System.Reactive;
using System.Reactive.Linq;
using System.Text.Json;
using Avalonia.Platform.Storage;
using EqualizerGUI.Models;
using EqualizerGUI.Services;
using ReactiveUI;

namespace EqualizerGUI.ViewModels;

/// <summary>One EQ band — bound to a Slider in the view.</summary>
public sealed class BandViewModel : ReactiveObject
{
    private double _gainDb;
    public double   Hz     { get; }
    public string   Label  { get; }

    public double GainDb
    {
        get => _gainDb;
        set => this.RaiseAndSetIfChanged(ref _gainDb, Math.Clamp(value, -12.0, 12.0));
    }

    public BandViewModel(double hz, double gainDb = 0.0)
    {
        Hz      = hz;
        Label   = hz >= 1000 ? $"{hz / 1000:0.#}k" : $"{hz:0}";
        _gainDb = gainDb;
    }
}

/// <summary>Main window view model.</summary>
public sealed class MainViewModel : ReactiveObject, IDisposable
{
    // ── Standard 10-band centres ──────────────────────────────────────────────
    private static readonly double[] BandHz =
        { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

    // ── Bands ─────────────────────────────────────────────────────────────────
    public ObservableCollection<BandViewModel> Bands { get; } = new();

    // ── Preamp ────────────────────────────────────────────────────────────────
    private double _preampDb;
    public double PreampDb
    {
        get => _preampDb;
        set => this.RaiseAndSetIfChanged(ref _preampDb, Math.Clamp(value, -20.0, 20.0));
    }

    // ── Enabled toggle ────────────────────────────────────────────────────────
    private bool _enabled = true;
    public bool Enabled
    {
        get => _enabled;
        set => this.RaiseAndSetIfChanged(ref _enabled, value);
    }

    // ── Status ────────────────────────────────────────────────────────────────
    private string _statusText = "Disconnected";
    public string StatusText
    {
        get => _statusText;
        private set => this.RaiseAndSetIfChanged(ref _statusText, value);
    }

    private bool _isConnected;
    public bool IsConnected
    {
        get => _isConnected;
        private set => this.RaiseAndSetIfChanged(ref _isConnected, value);
    }

    // ── Commands ──────────────────────────────────────────────────────────────
    public ReactiveCommand<Unit, Unit> ConnectCommand  { get; }
    public ReactiveCommand<Unit, Unit> ResetCommand    { get; }
    public ReactiveCommand<Unit, Unit> OpenPresetCommand  { get; }
    public ReactiveCommand<Unit, Unit> SavePresetCommand  { get; }
    public ReactiveCommand<Unit, Unit> RunCurveGenCommand { get; }

    // ── IPC ───────────────────────────────────────────────────────────────────
    private readonly IpcClient _ipc = new();
    private IDisposable?       _bandSubscription;
    private IDisposable?       _preampSubscription;
    private IDisposable?       _enabledSubscription;
    private readonly CancellationTokenSource _cts = new();

    // ── Avalonia storage provider (set by View) ───────────────────────────────
    public IStorageProvider? StorageProvider { get; set; }

    // ── Constructor ───────────────────────────────────────────────────────────
    public MainViewModel()
    {
        foreach (var hz in BandHz)
            Bands.Add(new BandViewModel(hz));

        ConnectCommand      = ReactiveCommand.CreateFromTask(ConnectAsync);
        ResetCommand        = ReactiveCommand.Create(ResetBands);
        OpenPresetCommand   = ReactiveCommand.CreateFromTask(OpenPresetAsync);
        SavePresetCommand   = ReactiveCommand.CreateFromTask(SavePresetAsync);
        RunCurveGenCommand  = ReactiveCommand.CreateFromTask(RunCurveGenAsync);

        // Subscribe to band changes (debounced 120 ms).
        var anyBandChanged = Bands
            .Select(b => b.WhenAnyValue(x => x.GainDb))
            .Merge();

        _bandSubscription = anyBandChanged
            .Throttle(TimeSpan.FromMilliseconds(120))
            .ObserveOn(RxApp.MainThreadScheduler)
            .SelectMany(_ => Observable.FromAsync(PushBandsAsync))
            .Subscribe();

        _preampSubscription = this.WhenAnyValue(x => x.PreampDb)
            .Skip(1)
            .Throttle(TimeSpan.FromMilliseconds(120))
            .SelectMany(db => Observable.FromAsync(ct => _ipc.SetPreampAsync(db, ct)))
            .Subscribe();

        _enabledSubscription = this.WhenAnyValue(x => x.Enabled)
            .Skip(1)
            .SelectMany(en => Observable.FromAsync(ct => _ipc.SetEnabledAsync(en, ct)))
            .Subscribe();
    }

    // ── IPC helpers ───────────────────────────────────────────────────────────

    private async Task ConnectAsync()
    {
        StatusText = "Connecting…";
        bool ok = await _ipc.ConnectAsync(_cts.Token);
        IsConnected = ok;
        StatusText  = ok ? "Connected" : "Connection failed";

        if (ok)
        {
            var state = await _ipc.GetStateAsync(_cts.Token);
            if (state?.GainsDb.Length == Bands.Count)
            {
                for (int i = 0; i < Bands.Count; i++)
                    Bands[i].GainDb = state.GainsDb[i];
                PreampDb = state.PreampDb;
                Enabled  = state.Enabled;
            }
        }
    }

    private async Task PushBandsAsync(CancellationToken ct = default)
    {
        if (!IsConnected) return;
        await _ipc.SetBandsAsync(Bands.Select(b => b.GainDb), ct);
    }

    // ── Band operations ───────────────────────────────────────────────────────

    private void ResetBands()
    {
        foreach (var b in Bands) b.GainDb = 0.0;
        PreampDb = 0.0;
    }

    // ── Preset I/O ────────────────────────────────────────────────────────────

    private async Task OpenPresetAsync()
    {
        if (StorageProvider is null) return;
        var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title           = "Open EQ Preset",
            AllowMultiple   = false,
            FileTypeFilter  = [new FilePickerFileType("EQ Preset") { Patterns = ["*.json"] }],
        });
        if (files.Count == 0) return;

        await using var stream = await files[0].OpenReadAsync();
        var preset = await JsonSerializer.DeserializeAsync<EqPreset>(stream);
        if (preset?.Bands is null || preset.Bands.Count != Bands.Count) return;

        for (int i = 0; i < Bands.Count; i++)
            Bands[i].GainDb = preset.Bands[i].GainDb;
        PreampDb = preset.PreampDb;
        StatusText = $"Preset loaded: {preset.Name}";
    }

    private async Task SavePresetAsync()
    {
        if (StorageProvider is null) return;
        var file = await StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions
        {
            Title           = "Save EQ Preset",
            SuggestedFileName = "preset.json",
            FileTypeChoices = [new FilePickerFileType("EQ Preset") { Patterns = ["*.json"] }],
        });
        if (file is null) return;

        var preset = new EqPreset
        {
            Name      = "Custom",
            PreampDb  = PreampDb,
            Bands     = Bands.Select((b, i) => new EqBand
            {
                Hz     = b.Hz,
                GainDb = b.GainDb,
            }).ToList(),
        };

        await using var stream = await file.OpenWriteAsync();
        await JsonSerializer.SerializeAsync(stream, preset,
            new JsonSerializerOptions { WriteIndented = true });
        StatusText = "Preset saved";
    }

    // ── CurveGen integration ──────────────────────────────────────────────────

    private async Task RunCurveGenAsync()
    {
        if (StorageProvider is null) return;
        var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Select Measurement WAV File",
            FileTypeFilter = [new FilePickerFileType("WAV Audio") { Patterns = ["*.wav"] }],
        });
        if (files.Count == 0) return;

        var inputPath  = files[0].TryGetLocalPath();
        if (inputPath is null) return;

        // Private per-run directory rather than a fixed name in the shared
        // temp dir. "{tmp}/eq_curve.json" is world-predictable: on Linux any
        // local account could pre-create or symlink that path and have this
        // process deserialise, or write through, whatever they chose.
        var workDir = Directory.CreateTempSubdirectory("eq-curvegen-").FullName;
        var outputPath = Path.Combine(workDir, "eq_curve.json");
        StatusText = "Running CurveGen…";

        try
        {
            var psi = new System.Diagnostics.ProcessStartInfo
            {
                FileName  = "eq-curvegen",
                Arguments = $"measure --input \"{inputPath}\" --output \"{outputPath}\" --harman",
                RedirectStandardOutput = true,
                RedirectStandardError  = true,
                UseShellExecute = false,
            };

            using var proc = System.Diagnostics.Process.Start(psi)!;

            // Drain both pipes concurrently with the wait. Redirected streams
            // that nobody reads fill their OS buffer and then block the child
            // on write — and eq-curvegen prints a full per-band table on every
            // run, so this deadlocked WaitForExitAsync on real inputs.
            var stdoutTask = proc.StandardOutput.ReadToEndAsync(_cts.Token);
            var stderrTask = proc.StandardError.ReadToEndAsync(_cts.Token);
            await proc.WaitForExitAsync(_cts.Token);
            var stdout = await stdoutTask;
            var stderr = await stderrTask;

            if (proc.ExitCode == 0 && File.Exists(outputPath))
            {
                await using var stream = File.OpenRead(outputPath);
                var preset = await JsonSerializer.DeserializeAsync<EqPreset>(stream);
                if (preset?.Bands is not null && preset.Bands.Count == Bands.Count)
                {
                    for (int i = 0; i < Bands.Count; i++)
                        Bands[i].GainDb = preset.Bands[i].GainDb;
                    PreampDb   = preset.PreampDb;
                    StatusText = "CurveGen complete — correction applied";
                    return;
                }
                StatusText = "CurveGen produced an unexpected preset shape";
                return;
            }

            // Surface the child's own diagnostics instead of guessing at the
            // cause; "check that eq-curvegen is installed" was wrong for every
            // failure mode except a missing binary.
            var detail = !string.IsNullOrWhiteSpace(stderr) ? stderr
                       : !string.IsNullOrWhiteSpace(stdout) ? stdout
                       : "no output";
            Console.Error.WriteLine($"[CurveGen] exit {proc.ExitCode}: {detail}");
            StatusText = $"CurveGen failed (exit {proc.ExitCode}) — see console for details";
        }
        catch (System.ComponentModel.Win32Exception)
        {
            StatusText = "CurveGen not found — is eq-curvegen installed and on PATH?";
        }
        catch (Exception ex)
        {
            StatusText = $"CurveGen error: {ex.Message}";
        }
        finally
        {
            try { Directory.Delete(workDir, recursive: true); } catch { /* best effort */ }
        }
    }

    // ── Dispose ───────────────────────────────────────────────────────────────

    public void Dispose()
    {
        _cts.Cancel();
        _bandSubscription?.Dispose();
        _preampSubscription?.Dispose();
        _enabledSubscription?.Dispose();
        _ipc.Dispose();
        _cts.Dispose();
    }
}
