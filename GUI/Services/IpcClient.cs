using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace EqualizerGUI.Services;

/// <summary>
/// Thin JSON-line IPC client for the eq-daemon.
/// - Linux/Mac: Unix domain socket at /tmp/eq-daemon.sock
/// - Windows:   Named pipe \\.\pipe\eq-daemon  (future)
/// </summary>
public sealed class IpcClient : IDisposable
{
    private const string UnixSocketPath = "/tmp/eq-daemon.sock";
    private const string WindowsPipeName = @"\\.\pipe\eq-daemon";

    private Socket?        _socket;
    private NetworkStream? _stream;
    private StreamReader?  _reader;

    public bool IsConnected => _socket?.Connected ?? false;

    // ── Connect / Disconnect ──────────────────────────────────────────────────

    public async Task<bool> ConnectAsync(CancellationToken ct = default)
    {
        try
        {
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                // TODO: implement Named Pipe client for Windows.
                return false;
            }

            _socket = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
            var endPoint = new UnixDomainSocketEndPoint(UnixSocketPath);
            await _socket.ConnectAsync(endPoint, ct);
            _stream = new NetworkStream(_socket, ownsSocket: false);
            _reader = new StreamReader(_stream, Encoding.UTF8);
            return true;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[IPC] Connect failed: {ex.Message}");
            return false;
        }
    }

    public void Dispose()
    {
        _reader?.Dispose();
        _stream?.Dispose();
        _socket?.Dispose();
    }

    // ── Commands ──────────────────────────────────────────────────────────────

    /// <summary>Push 10 band gains (dB) to the daemon.</summary>
    public Task<bool> SetBandsAsync(IEnumerable<double> gainsDb, CancellationToken ct = default)
    {
        var cmd = new { cmd = "set_bands", gains_db = gainsDb.ToArray() };
        return SendCommandAsync(cmd, ct);
    }

    /// <summary>Set global preamp.</summary>
    public Task<bool> SetPreampAsync(double gainDb, CancellationToken ct = default)
    {
        var cmd = new { cmd = "set_preamp", gain_db = gainDb };
        return SendCommandAsync(cmd, ct);
    }

    /// <summary>Enable or bypass the equalizer.</summary>
    public Task<bool> SetEnabledAsync(bool enabled, CancellationToken ct = default)
    {
        var cmd = new { cmd = "set_enabled", enabled };
        return SendCommandAsync(cmd, ct);
    }

    /// <summary>Request full daemon state.</summary>
    public async Task<DaemonState?> GetStateAsync(CancellationToken ct = default)
    {
        if (!IsConnected) return null;
        try
        {
            var cmd  = new { cmd = "get_state" };
            var line = JsonSerializer.Serialize(cmd) + "\n";
            var bytes = Encoding.UTF8.GetBytes(line);
            await _stream!.WriteAsync(bytes, ct);
            await _stream.FlushAsync(ct);

            var resp = await _reader!.ReadLineAsync(ct);
            if (resp is null) return null;
            return JsonSerializer.Deserialize<DaemonState>(resp);
        }
        catch { return null; }
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    private async Task<bool> SendCommandAsync<T>(T command, CancellationToken ct)
    {
        if (!IsConnected) return false;
        try
        {
            var line  = JsonSerializer.Serialize(command) + "\n";
            var bytes = Encoding.UTF8.GetBytes(line);
            await _stream!.WriteAsync(bytes, ct);
            await _stream.FlushAsync(ct);

            var resp = await _reader!.ReadLineAsync(ct);
            if (resp is null) return false;
            using var doc = JsonDocument.Parse(resp);
            return doc.RootElement.TryGetProperty("ok", out var ok) && ok.GetBoolean();
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[IPC] Command failed: {ex.Message}");
            return false;
        }
    }
}

/// <summary>Deserialisation target for the daemon's get_state response.</summary>
public sealed class DaemonState
{
    [System.Text.Json.Serialization.JsonPropertyName("gains_db")]    public double[] GainsDb    { get; set; } = Array.Empty<double>();
    [System.Text.Json.Serialization.JsonPropertyName("preamp_db")]   public double   PreampDb   { get; set; }
    [System.Text.Json.Serialization.JsonPropertyName("enabled")]     public bool     Enabled    { get; set; }
    [System.Text.Json.Serialization.JsonPropertyName("sample_rate")] public double   SampleRate { get; set; }
    [System.Text.Json.Serialization.JsonPropertyName("channels")]    public int      Channels   { get; set; }
}
