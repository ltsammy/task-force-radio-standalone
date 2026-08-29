using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using Tfrs.VoiceClient.Localization;

namespace Tfrs.VoiceClient.Networking;

internal enum DisconnectCause
{
    ClientRequested,
    ConnectionLost,
}

/// <summary>
/// UDP client for the TFRS voice relay protocol (docs/protocol-network.md). Owns a single
/// server connection: handshake with retry, a keepalive/RTT ping loop, voice frame send/receive,
/// and a locally-tracked roster driven by ClientJoined/ClientLeft broadcasts.
/// </summary>
internal sealed class VoiceNetworkClient : IAsyncDisposable
{
    private const int ConnectRetryCount = 5;
    private static readonly TimeSpan ConnectRetryTimeout = TimeSpan.FromSeconds(1.5);
    private static readonly TimeSpan PingInterval = TimeSpan.FromSeconds(5);
    private static readonly TimeSpan RosterRefreshInterval = TimeSpan.FromSeconds(20);
    // 3x the ping interval: tolerates a couple of dropped UDP pings without a false positive,
    // while still catching a genuinely dead/unreachable server (process killed, port closed) in
    // well under the server's own 20s default session timeout. UDP gives no other signal that the
    // peer is gone -- without this, a dead server just looks identical to "connected, silent".
    private static readonly TimeSpan ServerTimeout = TimeSpan.FromSeconds(15);

    private UdpClient? _udp;
    private CancellationTokenSource? _cts;
    private Task? _receiveLoopTask;
    private Task? _pingLoopTask;
    private Task? _watchdogTask;
    private TaskCompletionSource<(bool accepted, ConnectRejectReason reason)>? _pendingConnect;
    private ushort _voiceSequence;
    private readonly Stopwatch _clock = new();
    private DateTime _lastServerActivityUtc;

    public bool IsConnected { get; private set; }
    public uint SessionId { get; private set; }

    /// <summary>Server-dictated only (see ServerOptions.DebugForceAudible) — this client has no
    /// way to request or enable it itself.</summary>
    public bool ServerDebugForceAudible { get; private set; }

    /// <summary>The server's own version string, once known (see ConnectAccept) — logged on
    /// connect purely as a diagnostic, to catch a client/server version mismatch quickly.</summary>
    public string ServerVersion { get; private set; } = "";

    /// <summary>Fires synchronously from the receive loop the instant ConnectAccept is parsed —
    /// deliberately NOT via the awaited ConnectAsync return, because that continuation is queued
    /// (TaskCreationOptions.RunContinuationsAsynchronously) and can lose a race against the
    /// ClientJoined packets the server sends immediately after ConnectAccept: a subscriber that
    /// only reacted after ConnectAsync returned would still see sources created before it learned
    /// the flag was on.</summary>
    public event Action<bool>? DebugFlagReceived;

    public event Action? Connected;
    public event Action<string>? ConnectFailed;
    public event Action<DisconnectCause>? Disconnected;
    /// <summary>senderSessionId, sequence, isLastFrame, opus payload.</summary>
    public event Action<uint, ushort, bool, byte[]>? VoiceFrameReceived;
    public event Action<uint, string, string>? RemoteJoined;
    public event Action<uint>? RemoteLeft;
    public event Action<double>? RttMeasured;
    /// <summary>Another client's local radio-transmit state, relayed by the server: senderSessionId,
    /// active, freq, range (meters), sub(type). See docs/protocol-ipc-bridge.md "localTx"/"tx".</summary>
    public event Action<uint, bool, string, ushort, string>? RadioTxReceived;

    public async Task<bool> ConnectAsync(string host, int port, string password, string uid, string displayName, CancellationToken outerCt)
    {
        if (IsConnected)
            throw new InvalidOperationException("Already connected — call DisconnectAsync first.");

        IPAddress[] addresses;
        try
        {
            addresses = await Dns.GetHostAddressesAsync(host, AddressFamily.InterNetwork, outerCt);
            if (addresses.Length == 0)
                addresses = await Dns.GetHostAddressesAsync(host, outerCt);
        }
        catch (Exception ex)
        {
            ConnectFailed?.Invoke(Loc.Format("DnsFailed", ex.Message));
            return false;
        }
        if (addresses.Length == 0)
        {
            ConnectFailed?.Invoke(Loc.Get("HostNotResolved"));
            return false;
        }

        var udp = new UdpClient();
        try
        {
            udp.Connect(new IPEndPoint(addresses[0], port));
        }
        catch (Exception ex)
        {
            ConnectFailed?.Invoke($"Verbindung fehlgeschlagen: {ex.Message}");
            udp.Dispose();
            return false;
        }

        _udp = udp;
        _cts = CancellationTokenSource.CreateLinkedTokenSource(outerCt);
        _clock.Restart();
        _receiveLoopTask = Task.Run(() => ReceiveLoopAsync(_cts.Token));

        byte[] passwordHash = SHA256.HashData(Encoding.UTF8.GetBytes(password));
        byte[] request = BuildConnectRequest(passwordHash, uid, displayName);

        for (int attempt = 1; attempt <= ConnectRetryCount; attempt++)
        {
            if (outerCt.IsCancellationRequested) break;

            _pendingConnect = new TaskCompletionSource<(bool, ConnectRejectReason)>(TaskCreationOptions.RunContinuationsAsynchronously);
            try
            {
                await udp.SendAsync(request, outerCt);
            }
            catch (Exception ex)
            {
                ConnectFailed?.Invoke(Loc.Format("SendFailed", ex.Message));
                break;
            }

            var completed = await Task.WhenAny(_pendingConnect.Task, Task.Delay(ConnectRetryTimeout, outerCt));
            if (completed == _pendingConnect.Task)
            {
                var (accepted, reason) = await _pendingConnect.Task;
                if (accepted)
                {
                    IsConnected = true;
                    _lastServerActivityUtc = DateTime.UtcNow;
                    _pingLoopTask = Task.Run(() => PingLoopAsync(_cts.Token));
                    _watchdogTask = Task.Run(() => WatchdogLoopAsync(_cts.Token));
                    Connected?.Invoke();
                    return true;
                }

                ConnectFailed?.Invoke(DescribeReject(reason));
                await TeardownAsync(notify: false);
                return false;
            }
            // timeout — loop and retry
        }

        ConnectFailed?.Invoke(Loc.Get("ServerTimeout"));
        await TeardownAsync(notify: false);
        return false;
    }

    public async Task DisconnectAsync()
    {
        if (_udp is not null && IsConnected)
        {
            try
            {
                await _udp.SendAsync(new byte[] { (byte)PacketType.Disconnect }, CancellationToken.None);
            }
            catch
            {
                // best-effort courtesy notice — we're tearing down regardless
            }
        }

        await TeardownAsync(notify: true, cause: DisconnectCause.ClientRequested);
    }

    public void SendVoiceFrame(ReadOnlySpan<byte> opusPayload, bool isLast)
    {
        if (_udp is null || !IsConnected) return;
        if (opusPayload.Length > Protocol.MaxOpusFrameLength) return;

        Span<byte> buf = stackalloc byte[1 + 2 + 1 + Protocol.MaxOpusFrameLength];
        var writer = new PacketWriter(buf);
        writer.WriteByte((byte)PacketType.VoiceUp);
        writer.WriteUInt16(_voiceSequence++);
        writer.WriteByte((byte)(isLast ? VoiceFlags.LastFrame : VoiceFlags.None));
        writer.WriteBytes(opusPayload);
        _ = SendSafeAsync(writer.Written.ToArray());
    }

    /// <summary>Broadcasts (via the server) what we're currently transmitting on radio, so every
    /// other client can forward it to its own local extension. Fire-and-forget, like voice frames
    /// — the extension's own 1.5s expiry (see State.cpp's kTxExpiry) tolerates a dropped packet.</summary>
    public void SendRadioTx(bool active, string freq, ushort range, string sub)
    {
        if (_udp is null || !IsConnected) return;

        Span<byte> buf = stackalloc byte[1 + 1 + 1 + Protocol.MaxFreqLength + 2 + 1 + Protocol.MaxSubtypeLength];
        var writer = new PacketWriter(buf);
        writer.WriteByte((byte)PacketType.RadioTxUpdate);
        writer.WriteByte((byte)(active ? 1 : 0));
        writer.WriteString8(Truncate(freq, Protocol.MaxFreqLength));
        writer.WriteUInt16(range);
        writer.WriteString8(Truncate(sub, Protocol.MaxSubtypeLength));
        _ = SendSafeAsync(writer.Written.ToArray());
    }

    public void RequestRoster()
    {
        if (_udp is null || !IsConnected) return;
        _ = SendSafeAsync([(byte)PacketType.RosterRequest]);
    }

    private async Task SendSafeAsync(byte[] data)
    {
        try
        {
            if (_udp is not null)
                await _udp.SendAsync(data);
        }
        catch (SocketException)
        {
            // transient — the periodic ping/timeout logic will surface a real disconnect
        }
        catch (ObjectDisposedException)
        {
        }
    }

    private async Task ReceiveLoopAsync(CancellationToken ct)
    {
        if (_udp is null) return;
        while (!ct.IsCancellationRequested)
        {
            UdpReceiveResult result;
            try
            {
                result = await _udp.ReceiveAsync(ct);
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (ObjectDisposedException)
            {
                break;
            }
            catch (SocketException)
            {
                continue;
            }

            try
            {
                HandlePacket(result.Buffer);
            }
            catch (Exception)
            {
                // malformed packet from the server — ignore and keep the connection alive
            }
        }
    }

    private void HandlePacket(byte[] data)
    {
        if (data.Length < 1) return;
        _lastServerActivityUtc = DateTime.UtcNow; // any datagram from the server counts as proof of life
        var type = (PacketType)data[0];
        var reader = new PacketReader(data.AsSpan(1));

        switch (type)
        {
            case PacketType.ConnectAccept:
            {
                uint sessionId = reader.ReadUInt32();
                SessionId = sessionId;
                if (reader.Remaining >= 2) reader.ReadUInt16(); // maxClients, informational only
                ServerDebugForceAudible = reader.Remaining >= 1 && reader.ReadByte() != 0;
                if (reader.Remaining >= 1)
                {
                    try { ServerVersion = reader.ReadString8(Protocol.MaxVersionLength); }
                    catch (InvalidDataException) { /* older/malformed field — leave empty */ }
                }
                DebugFlagReceived?.Invoke(ServerDebugForceAudible);
                _pendingConnect?.TrySetResult((true, default));
                break;
            }
            case PacketType.ConnectReject:
            {
                var reason = (ConnectRejectReason)reader.ReadByte();
                _pendingConnect?.TrySetResult((false, reason));
                break;
            }
            case PacketType.Pong:
            {
                uint echoed = reader.ReadUInt32();
                double rtt = _clock.Elapsed.TotalMilliseconds - echoed;
                if (rtt >= 0) RttMeasured?.Invoke(rtt);
                break;
            }
            case PacketType.VoiceDown:
            {
                uint senderSessionId = reader.ReadUInt32();
                ushort sequence = reader.ReadUInt16();
                byte flags = reader.ReadByte();
                var opus = reader.ReadRemaining().ToArray();
                bool isLast = ((VoiceFlags)flags).HasFlag(VoiceFlags.LastFrame);
                VoiceFrameReceived?.Invoke(senderSessionId, sequence, isLast, opus);
                break;
            }
            case PacketType.ClientJoined:
            {
                uint sessionId = reader.ReadUInt32();
                string uid = reader.ReadString8(Protocol.MaxUidLength);
                string name = reader.ReadString8(Protocol.MaxNameLength);
                RemoteJoined?.Invoke(sessionId, uid, name);
                break;
            }
            case PacketType.ClientLeft:
            {
                uint sessionId = reader.ReadUInt32();
                RemoteLeft?.Invoke(sessionId);
                break;
            }
            case PacketType.RadioTxBroadcast:
            {
                uint senderSessionId = reader.ReadUInt32();
                bool active = reader.ReadByte() != 0;
                string freq = reader.ReadString8(Protocol.MaxFreqLength);
                ushort range = reader.ReadUInt16();
                string sub = reader.ReadString8(Protocol.MaxSubtypeLength);
                RadioTxReceived?.Invoke(senderSessionId, active, freq, range, sub);
                break;
            }
        }
    }

    private async Task PingLoopAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(PingInterval, ct);
            }
            catch (OperationCanceledException)
            {
                break;
            }

            if (_udp is null) break;

            _ = SendSafeAsync(BuildPingPacket((uint)_clock.Elapsed.TotalMilliseconds));

            RequestRoster(); // cheap safety net against dropped ClientJoined/Left broadcasts
        }
    }

    /// <summary>UDP gives no notification when the remote end disappears (process killed, box
    /// rebooted, firewall change) -- without this, a dead server looks identical to "connected,
    /// nobody's talking" forever: the client just keeps sending pings and mic frames into the
    /// void. Declares the connection lost if nothing at all has arrived from the server (Pong,
    /// VoiceDown, ClientJoined/Left -- anything) for ServerTimeout.</summary>
    private async Task WatchdogLoopAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(TimeSpan.FromSeconds(2), ct);
            }
            catch (OperationCanceledException)
            {
                break;
            }

            if (DateTime.UtcNow - _lastServerActivityUtc > ServerTimeout)
            {
                _ = TeardownAsync(notify: true, cause: DisconnectCause.ConnectionLost);
                break;
            }
        }
    }

    private static byte[] BuildPingPacket(uint timestampMs)
    {
        Span<byte> buf = stackalloc byte[5];
        var writer = new PacketWriter(buf);
        writer.WriteByte((byte)PacketType.Ping);
        writer.WriteUInt32(timestampMs);
        return writer.Written.ToArray();
    }

    private static byte[] BuildConnectRequest(byte[] passwordHash, string uid, string displayName)
    {
        Span<byte> buf = stackalloc byte[1 + 1 + 32 + 1 + Protocol.MaxUidLength + 1 + Protocol.MaxNameLength];
        var writer = new PacketWriter(buf);
        writer.WriteByte((byte)PacketType.ConnectRequest);
        writer.WriteByte(Protocol.VersionMajor);
        writer.WriteBytes(passwordHash);
        writer.WriteString8(Truncate(uid, Protocol.MaxUidLength));
        writer.WriteString8(Truncate(displayName, Protocol.MaxNameLength));
        return writer.Written.ToArray();
    }

    private static string Truncate(string? value, int maxUtf8Bytes)
    {
        // Defense in depth: a null here (e.g. a default-initialized record struct field) must not
        // crash a low-level packet-writing helper -- Encoding.UTF8.GetByteCount(null) throws
        // ArgumentNullException, which took down every caller of this (SendRadioTx on every idle
        // "units" message, in practice) before the actual null was fixed at its source.
        value ??= string.Empty;
        while (Encoding.UTF8.GetByteCount(value) > maxUtf8Bytes)
            value = value[..^1];
        return value;
    }

    private static string DescribeReject(ConnectRejectReason reason) => reason switch
    {
        ConnectRejectReason.BadPassword => Loc.Get("BadPassword"),
        ConnectRejectReason.ServerFull => Loc.Get("ServerFull"),
        ConnectRejectReason.VersionMismatch => Loc.Get("VersionMismatch"),
        _ => Loc.Get("ConnectionRejected"),
    };

    private async Task TeardownAsync(bool notify, DisconnectCause cause = DisconnectCause.ConnectionLost)
    {
        bool wasConnected = IsConnected;
        IsConnected = false;
        SessionId = 0;

        _cts?.Cancel();
        if (_receiveLoopTask is not null) await SafeAwaitAsync(_receiveLoopTask);
        if (_pingLoopTask is not null) await SafeAwaitAsync(_pingLoopTask);
        // Not awaited when the watchdog itself is the caller (it fires this fire-and-forget then
        // immediately breaks out of its own loop) -- awaiting our own currently-running task here
        // would just wait on a task that's already about to complete, not deadlock, but there's
        // nothing to gain by delaying teardown on it either way.
        if (_watchdogTask is not null) await SafeAwaitAsync(_watchdogTask);

        _udp?.Dispose();
        _udp = null;
        _cts?.Dispose();
        _cts = null;
        _receiveLoopTask = null;
        _pingLoopTask = null;
        _watchdogTask = null;

        if (notify && wasConnected)
            Disconnected?.Invoke(cause);
    }

    private static async Task SafeAwaitAsync(Task task)
    {
        try { await task; } catch { /* observed on purpose */ }
    }

    public async ValueTask DisposeAsync()
    {
        await TeardownAsync(notify: false);
    }
}
