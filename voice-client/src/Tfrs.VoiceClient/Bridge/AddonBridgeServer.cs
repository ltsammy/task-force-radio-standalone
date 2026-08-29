using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;

namespace Tfrs.VoiceClient.Bridge;

internal sealed record UnitEntry(string Uid, float Gain, float Azimuth, bool Muted, string Fx, float ErrorLevel);

/// <summary>Our own local radio-transmit state, as reported by the extension's "localTx" field
/// (see docs/protocol-ipc-bridge.md). Freq/Range/Sub are meaningless when Active is false.</summary>
internal readonly record struct LocalTxState(bool Active, string Freq, int Range, string Sub);

/// <summary>One entry for the "tx" message sent to the extension — see AddonBridgeServer.SendTxAsync.</summary>
internal readonly record struct RemoteTxEntry(string Uid, string Freq, int Range, string Sub);

/// <summary>
/// Named-pipe server for the Arma extension (see docs/protocol-ipc-bridge.md). Accepts one client
/// connection at a time and re-listens after disconnect — the extension only exists while Arma is
/// running and may (re)connect at any point relative to this process's lifetime.
/// </summary>
internal sealed class AddonBridgeServer : IAsyncDisposable
{
    private const string PipeName = "TFRS_VoiceBridge";

    private readonly CancellationTokenSource _cts = new();
    private readonly Task _acceptLoopTask;
    private NamedPipeServerStream? _activePipe;
    private readonly SemaphoreSlim _writeLock = new(1, 1);

    public event Action<IReadOnlyList<UnitEntry>>? UnitsReceived;
    public event Action<bool?>? LocalOverrideReceived;
    /// <summary>The local player's real Steam UID (getPlayerUID), reported by the extension once
    /// it knows it — see the "myUid" field on the "units" message in docs/protocol-ipc-bridge.md.
    /// Only fires with a non-empty value.</summary>
    public event Action<string>? LocalUidReceived;
    /// <summary>Fires on every "units" message with the current local radio-transmit state
    /// (Active=false most of the time) — see LocalTxState.</summary>
    public event Action<LocalTxState>? LocalTxChanged;
    public event Action? ExtensionConnected;
    public event Action? ExtensionDisconnected;

    public AddonBridgeServer()
    {
        _acceptLoopTask = Task.Run(() => AcceptLoopAsync(_cts.Token));
    }

    private async Task AcceptLoopAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            var pipe = new NamedPipeServerStream(PipeName, PipeDirection.InOut, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous);
            try
            {
                await pipe.WaitForConnectionAsync(ct);
            }
            catch (OperationCanceledException)
            {
                pipe.Dispose();
                break;
            }
            catch (IOException)
            {
                pipe.Dispose();
                continue;
            }

            _activePipe = pipe;
            ExtensionConnected?.Invoke();
            await ReadLoopAsync(pipe, ct);
            _activePipe = null;
            ExtensionDisconnected?.Invoke();
            pipe.Dispose();
        }
    }

    private async Task ReadLoopAsync(NamedPipeServerStream pipe, CancellationToken ct)
    {
        using var reader = new StreamReader(pipe, Encoding.UTF8, detectEncodingFromByteOrderMarks: false, leaveOpen: true);
        while (!ct.IsCancellationRequested && pipe.IsConnected)
        {
            string? line;
            try
            {
                line = await reader.ReadLineAsync(ct);
            }
            catch (IOException)
            {
                break;
            }
            catch (OperationCanceledException)
            {
                break;
            }

            if (line is null) break; // client disconnected
            if (line.Length == 0) continue;

            try
            {
                HandleLine(line);
            }
            catch (JsonException)
            {
                // malformed line from the extension — ignore and keep the connection alive
            }
        }
    }

    private void HandleLine(string line)
    {
        using var doc = JsonDocument.Parse(line);
        if (!doc.RootElement.TryGetProperty("t", out var typeProp)) return;

        switch (typeProp.GetString())
        {
            case "units":
                HandleUnits(doc.RootElement);
                break;
            case "local":
                HandleLocal(doc.RootElement);
                break;
        }
    }

    private void HandleUnits(JsonElement root)
    {
        var list = new List<UnitEntry>();
        if (root.TryGetProperty("u", out var arr) && arr.ValueKind == JsonValueKind.Array)
        {
            foreach (var e in arr.EnumerateArray())
            {
                string uid = e.TryGetProperty("uid", out var uidProp) ? uidProp.GetString() ?? "" : "";
                if (uid.Length == 0) continue;
                float gain = e.TryGetProperty("gain", out var g) ? g.GetSingle() : 0f;
                float az = e.TryGetProperty("az", out var a) ? a.GetSingle() : 0f;
                bool muted = e.TryGetProperty("muted", out var m) && m.GetBoolean();
                string fx = e.TryGetProperty("fx", out var f) ? f.GetString() ?? "direct" : "direct";
                float err = e.TryGetProperty("err", out var er) ? er.GetSingle() : 0f;
                list.Add(new UnitEntry(uid, gain, az, muted, fx, err));
            }
        }
        if (root.TryGetProperty("myUid", out var myUidProp))
        {
            string myUid = myUidProp.GetString() ?? "";
            if (myUid.Length > 0) LocalUidReceived?.Invoke(myUid);
        }

        LocalTxState localTx = default;
        if (root.TryGetProperty("localTx", out var txProp) && txProp.ValueKind == JsonValueKind.Object)
        {
            bool active = txProp.TryGetProperty("active", out var a2) && a2.GetBoolean();
            string freq = txProp.TryGetProperty("freq", out var f2) ? f2.GetString() ?? "" : "";
            int range = txProp.TryGetProperty("range", out var r2) ? r2.GetInt32() : 0;
            string sub = txProp.TryGetProperty("sub", out var s2) ? s2.GetString() ?? "" : "";
            localTx = new LocalTxState(active, freq, range, sub);
        }
        LocalTxChanged?.Invoke(localTx);

        UnitsReceived?.Invoke(list);
    }

    private void HandleLocal(JsonElement root)
    {
        if (!root.TryGetProperty("transmitOverride", out var prop)) return;
        bool? value = prop.ValueKind switch
        {
            JsonValueKind.True => true,
            JsonValueKind.False => false,
            _ => null,
        };
        LocalOverrideReceived?.Invoke(value);
    }

    public async Task SendStatusAsync(bool connected, uint? sessionId, bool micMuted, bool speakerMuted, bool transmitting, string? error)
    {
        var sb = new StringBuilder();
        sb.Append("{\"t\":\"status\",\"connected\":").Append(connected ? "true" : "false");
        sb.Append(",\"sessionId\":").Append(sessionId.HasValue ? sessionId.Value.ToString() : "null");
        sb.Append(",\"micMuted\":").Append(micMuted ? "true" : "false");
        sb.Append(",\"speakerMuted\":").Append(speakerMuted ? "true" : "false");
        sb.Append(",\"transmitting\":").Append(transmitting ? "true" : "false");
        sb.Append(",\"error\":").Append(error is null ? "null" : JsonSerializer.Serialize(error));
        sb.Append('}');
        await WriteLineAsync(sb.ToString());
    }

    /// <summary>Full snapshot of every remote player currently transmitting on radio (see
    /// docs/protocol-ipc-bridge.md's "tx" message) — anyone missing is treated as no longer
    /// transmitting, same semantics as "units".</summary>
    public async Task SendTxAsync(IReadOnlyList<RemoteTxEntry> entries)
    {
        var sb = new StringBuilder();
        sb.Append("{\"t\":\"tx\",\"u\":[");
        for (int i = 0; i < entries.Count; i++)
        {
            if (i > 0) sb.Append(',');
            var e = entries[i];
            sb.Append("{\"uid\":").Append(JsonSerializer.Serialize(e.Uid));
            sb.Append(",\"freq\":").Append(JsonSerializer.Serialize(e.Freq));
            sb.Append(",\"range\":").Append(e.Range);
            sb.Append(",\"sub\":").Append(JsonSerializer.Serialize(e.Sub)).Append('}');
        }
        sb.Append("]}");
        await WriteLineAsync(sb.ToString());
    }

    private async Task WriteLineAsync(string json)
    {
        var pipe = _activePipe;
        if (pipe is null || !pipe.IsConnected) return;

        await _writeLock.WaitAsync();
        try
        {
            byte[] bytes = Encoding.UTF8.GetBytes(json + "\n");
            await pipe.WriteAsync(bytes);
            await pipe.FlushAsync();
        }
        catch (IOException)
        {
            // extension disconnected mid-write — the accept loop will notice and re-listen
        }
        finally
        {
            _writeLock.Release();
        }
    }

    public async ValueTask DisposeAsync()
    {
        _cts.Cancel();
        _activePipe?.Dispose();
        try { await _acceptLoopTask; } catch { /* observed on purpose */ }
        _writeLock.Dispose();
        _cts.Dispose();
    }
}
