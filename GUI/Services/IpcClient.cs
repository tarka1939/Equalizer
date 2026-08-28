using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace EqualizerGUI.Services;

/// <summary>
/// Thin JSON-line IPC client for the eq-daemon.
/// - Linux/Mac: Unix domain socket at /tmp/eq-daemon.sock
/// - Windows:   Named pipe \\.\pipe\eq-daemon  (future)
///
/// The protocol is strictly request/response over a single connection, so
/// every exchange is serialised through <see cref="_ioGate"/>. Without it,
/// MainViewModel's three independent Rx subscriptions (bands, preamp,
/// enabled) could each be inside a write/ReadLine pair on the same stream at
/// the same time: their requests interleave on the wire and each caller reads
/// whichever reply happens to arrive first. Moving a slider while toggling
/// the enable switch was enough to trigger it.
/// </summary>
public sealed class IpcClient : IDisposable
{
    private const string UnixSocketPath = "/tmp/eq-daemon.sock";
    private const string WindowsPipeName = @"\\.\pipe\eq-daemon";

    private Socket?        _socket;
    private NetworkStream? _stream;
    private StreamReader?  _reader;

    private readonly SemaphoreSlim _ioGate = new(1, 1);

    public bool IsConnected => _socket?.Connected ?? false;

    // ── Connect / Disconnect ──────────────────────────────────────────────────

    public async Task<bool> ConnectAsync(CancellationToken ct = default)
    {
        await _ioGate.WaitAsync(ct);
        try
        {
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                // TODO: implement Named Pipe client for Windows.
                return false;
            }

            // Release any previous connection first. Reconnecting used to
            // overwrite these three fields and leak the old socket, stream
            // and reader (and the underlying file descriptor with them).
            CloseConnection();

            var socket = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
            try
            {
                var endPoint = new UnixDomainSocketEndPoint(UnixSocketPath);
                await socket.ConnectAsync(endPoint, ct);
            }
            catch
            {
                socket.Dispose();
                throw;
            }

            _socket = socket;
            _stream = new NetworkStream(_socket, ownsSocket: false);
            _reader = new StreamReader(_stream, Encoding.UTF8);
            return true;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[IPC] Connect failed: {ex.Message}");
            return false;
        }
        finally
        {
            _ioGate.Release();
        }
    }

    private void CloseConnection()
    {
        _reader?.Dispose();
        _stream?.Dispose();
        _socket?.Dispose();
        _reader = null;
        _stream = null;
        _socket = null;
    }

    public void Dispose()
    {
        CloseConnection();
        _ioGate.Dispose();
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
        var resp = await ExchangeAsync(new { cmd = "get_state" }, ct);
        if (resp is null) return null;
        try { return JsonSerializer.Deserialize<DaemonState>(resp); }
        catch { return null; }
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    /// <summary>
    /// Write one command line and read exactly one response line, holding the
    /// I/O gate for the whole round trip so concurrent callers can't
    /// interleave on the shared stream. Returns null on any failure.
    /// </summary>
    private async Task<string?> ExchangeAsync<T>(T command, CancellationToken ct)
    {
        if (!IsConnected) return null;

        await _ioGate.WaitAsync(ct);
        try
        {
            // Re-check inside the gate: another caller may have hit an error
            // and torn the connection down while we were queued.
            if (_stream is null || _reader is null) return null;

            var line  = JsonSerializer.Serialize(command) + "\n";
            var bytes = Encoding.UTF8.GetBytes(line);
            await _stream.WriteAsync(bytes, ct);
            await _stream.FlushAsync(ct);

            return await _reader.ReadLineAsync(ct);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[IPC] Command failed: {ex.Message}");
            return null;
        }
        finally
        {
            _ioGate.Release();
        }
    }

    private async Task<bool> SendCommandAsync<T>(T command, CancellationToken ct)
    {
        var resp = await ExchangeAsync(command, ct);
        if (resp is null) return false;
        try
        {
            using var doc = JsonDocument.Parse(resp);
            return doc.RootElement.TryGetProperty("ok", out var ok) && ok.GetBoolean();
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[IPC] Malformed response: {ex.Message}");
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
    [System.Text.Json.Serialization.JsonPropertyName("fir_length")]  public int      FirLength  { get; set; }
}
