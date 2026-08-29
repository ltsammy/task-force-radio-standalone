using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;

namespace Tfrs.VoiceServer;

internal sealed class RelayServer : IAsyncDisposable
{
    private readonly ServerOptions _options;
    private readonly byte[] _expectedPasswordHash;
    private readonly Socket _socket;
    private readonly ConcurrentDictionary<IPEndPoint, ClientSession> _sessionsByEndPoint = new();
    private readonly ConcurrentDictionary<uint, ClientSession> _sessionsById = new();
    private readonly ConcurrentDictionary<string, ClientSession> _sessionsByUid = new();
    private uint _nextSessionId = 1;

    public RelayServer(ServerOptions options)
    {
        _options = options;
        _expectedPasswordHash = SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(options.Password));

        // Plain IPv4: keeps this predictable across Docker/Coolify network setups where
        // IPv6 dual-stack inside a container is often unconfigured or disabled.
        _socket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        _socket.Bind(new IPEndPoint(IPAddress.Any, options.Port));
    }

    public async Task RunAsync(CancellationToken ct)
    {
        Log($"TFRS voice relay listening on UDP port {_options.Port} (max clients: {_options.MaxClients}, timeout: {_options.TimeoutSeconds}s)");
        if (string.IsNullOrEmpty(_options.Password))
            Log("WARNING: no password configured — server accepts any client.");
        if (_options.DebugForceAudible)
            Log("!!! DEBUG_FORCE_AUDIBLE is ON: every client hears every other client at full volume, " +
                "ignoring the Arma addon's distance/gain logic entirely. Testing only — turn this off " +
                "for real play.");

        _ = Task.Run(() => TimeoutSweepLoopAsync(ct), ct);

        var buffer = new byte[2048];
        while (!ct.IsCancellationRequested)
        {
            SocketReceiveFromResult result;
            try
            {
                result = await _socket.ReceiveFromAsync(buffer, SocketFlags.None, new IPEndPoint(IPAddress.Any, 0), ct);
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (SocketException)
            {
                continue; // transient — e.g. ICMP port-unreachable surfaced on some platforms
            }

            if (result.ReceivedBytes < 1 || result.RemoteEndPoint is not IPEndPoint remote)
                continue;

            try
            {
                HandlePacket(buffer.AsSpan(0, result.ReceivedBytes), remote);
            }
            catch (Exception ex)
            {
                Log($"Malformed packet from {remote}: {ex.Message}");
            }
        }
    }

    private void HandlePacket(ReadOnlySpan<byte> data, IPEndPoint remote)
    {
        var packetType = (PacketType)data[0];
        var reader = new PacketReader(data[1..]);

        switch (packetType)
        {
            case PacketType.ConnectRequest:
                HandleConnectRequest(ref reader, remote);
                break;
            case PacketType.RosterRequest:
                HandleRosterRequest(remote);
                break;
            case PacketType.Ping:
                HandlePing(ref reader, remote);
                break;
            case PacketType.VoiceUp:
                HandleVoiceUp(ref reader, remote);
                break;
            case PacketType.RadioTxUpdate:
                HandleRadioTxUpdate(ref reader, remote);
                break;
            case PacketType.Disconnect:
                HandleDisconnect(remote);
                break;
        }
    }

    private void HandleConnectRequest(ref PacketReader reader, IPEndPoint remote)
    {
        byte version = reader.ReadByte();
        if (version != Protocol.VersionMajor)
        {
            SendReject(remote, ConnectRejectReason.VersionMismatch);
            return;
        }

        if (reader.Remaining < 32)
        {
            SendReject(remote, ConnectRejectReason.BadRequest);
            return;
        }
        var passwordHash = reader.ReadBytesFixed(32);
        string uid, name;
        try
        {
            uid = reader.ReadString8(Protocol.MaxUidLength);
            name = reader.ReadString8(Protocol.MaxNameLength);
        }
        catch (InvalidDataException)
        {
            SendReject(remote, ConnectRejectReason.BadRequest);
            return;
        }

        if (string.IsNullOrEmpty(uid))
        {
            SendReject(remote, ConnectRejectReason.BadRequest);
            return;
        }

        if (!CryptographicOperations.FixedTimeEquals(passwordHash, _expectedPasswordHash))
        {
            SendReject(remote, ConnectRejectReason.BadPassword);
            return;
        }

        // Reconnect: drop any stale session for the same UID (different endpoint, e.g. client restarted).
        if (_sessionsByUid.TryRemove(uid, out var stale))
        {
            _sessionsByEndPoint.TryRemove(stale.EndPoint, out _);
            _sessionsById.TryRemove(stale.SessionId, out _);
            BroadcastClientLeft(stale.SessionId, except: null);
        }

        if (_sessionsById.Count >= _options.MaxClients)
        {
            SendReject(remote, ConnectRejectReason.ServerFull);
            return;
        }

        var session = new ClientSession
        {
            SessionId = _nextSessionId++,
            EndPoint = remote,
            Uid = uid,
            Name = string.IsNullOrEmpty(name) ? uid : name,
        };
        _sessionsByEndPoint[remote] = session;
        _sessionsById[session.SessionId] = session;
        _sessionsByUid[uid] = session;

        Log($"Client connected: session={session.SessionId} uid={uid} name={session.Name} from={remote}");

        SendAccept(session);

        // Tell the new client about everyone already present.
        foreach (var existing in _sessionsById.Values)
        {
            if (existing.SessionId == session.SessionId) continue;
            SendClientJoined(remote, existing);
        }

        // Tell everyone else about the new client.
        BroadcastClientJoined(session, except: session.SessionId);
    }

    private void HandleRosterRequest(IPEndPoint remote)
    {
        if (!_sessionsByEndPoint.TryGetValue(remote, out var requester))
            return;

        foreach (var existing in _sessionsById.Values)
        {
            if (existing.SessionId == requester.SessionId) continue;
            SendClientJoined(remote, existing);
        }
    }

    private void HandlePing(ref PacketReader reader, IPEndPoint remote)
    {
        uint timestamp = reader.ReadUInt32();
        // Used to Pong unconditionally, even for a sender with no registered session. That
        // defeated the client's own connection-loss watchdog after a server restart: the new
        // process has no memory of the old session, but kept happily replying to its stray pings
        // anyway, which the client's "any packet = still alive" check took as proof the connection
        // was fine -- so it never noticed the restart, stayed "connected" with a stale roster, and
        // never reconnected. A stale client with no session must get silence here so its own
        // ping-timeout logic can do its job instead.
        if (!_sessionsByEndPoint.TryGetValue(remote, out var session)) return;
        session.LastSeenUtc = DateTime.UtcNow;

        Span<byte> outBuf = stackalloc byte[6];
        var writer = new PacketWriter(outBuf);
        writer.WriteByte((byte)PacketType.Pong);
        writer.WriteUInt32(timestamp);
        Send(writer.Written, remote);
    }

    private void HandleVoiceUp(ref PacketReader reader, IPEndPoint remote)
    {
        if (!_sessionsByEndPoint.TryGetValue(remote, out var sender))
            return; // not connected — silently drop

        sender.LastSeenUtc = DateTime.UtcNow;

        ushort sequence = reader.ReadUInt16();
        byte flags = reader.ReadByte();
        var opus = reader.ReadRemaining();
        if (opus.Length > Protocol.MaxOpusFrameLength)
            return;

        sender.LastVoiceSequence = sequence;

        Span<byte> outBuf = stackalloc byte[1 + 4 + 2 + 1 + Protocol.MaxOpusFrameLength];
        var writer = new PacketWriter(outBuf);
        writer.WriteByte((byte)PacketType.VoiceDown);
        writer.WriteUInt32(sender.SessionId);
        writer.WriteUInt16(sequence);
        writer.WriteByte(flags);
        writer.WriteBytes(opus);
        var packet = writer.Written.ToArray(); // detach from stack before the async fan-out below

        foreach (var target in _sessionsById.Values)
        {
            if (target.SessionId == sender.SessionId) continue;
            FireAndForgetSend(packet, target.EndPoint);
        }
    }

    /// <summary>Pure relay, same as HandleVoiceUp: no server-side understanding of frequencies or
    /// who can hear whom — every receiving client's own Arma extension decides that, this just
    /// needs to reach everyone else. See docs/protocol-ipc-bridge.md's "localTx"/"tx" messages.</summary>
    private void HandleRadioTxUpdate(ref PacketReader reader, IPEndPoint remote)
    {
        if (!_sessionsByEndPoint.TryGetValue(remote, out var sender)) return;
        sender.LastSeenUtc = DateTime.UtcNow;

        byte active;
        string freq, sub;
        ushort range;
        try
        {
            active = reader.ReadByte();
            freq = reader.ReadString8(Protocol.MaxFreqLength);
            range = reader.ReadUInt16();
            sub = reader.ReadString8(Protocol.MaxSubtypeLength);
        }
        catch (InvalidDataException)
        {
            return;
        }

        Span<byte> outBuf = stackalloc byte[1 + 4 + 1 + 1 + Protocol.MaxFreqLength + 2 + 1 + Protocol.MaxSubtypeLength];
        var writer = new PacketWriter(outBuf);
        writer.WriteByte((byte)PacketType.RadioTxBroadcast);
        writer.WriteUInt32(sender.SessionId);
        writer.WriteByte(active);
        writer.WriteString8(freq);
        writer.WriteUInt16(range);
        writer.WriteString8(sub);
        var packet = writer.Written.ToArray();

        foreach (var target in _sessionsById.Values)
        {
            if (target.SessionId == sender.SessionId) continue;
            FireAndForgetSend(packet, target.EndPoint);
        }
    }

    private void HandleDisconnect(IPEndPoint remote)
    {
        if (!_sessionsByEndPoint.TryRemove(remote, out var session))
            return;

        _sessionsById.TryRemove(session.SessionId, out _);
        _sessionsByUid.TryRemove(session.Uid, out _);
        Log($"Client disconnected: session={session.SessionId} uid={session.Uid}");
        BroadcastClientLeft(session.SessionId, except: null);
    }

    private async Task TimeoutSweepLoopAsync(CancellationToken ct)
    {
        var interval = TimeSpan.FromSeconds(5);
        while (!ct.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(interval, ct);
            }
            catch (OperationCanceledException)
            {
                break;
            }

            var cutoff = DateTime.UtcNow - TimeSpan.FromSeconds(_options.TimeoutSeconds);
            foreach (var session in _sessionsById.Values)
            {
                if (session.LastSeenUtc >= cutoff) continue;

                _sessionsByEndPoint.TryRemove(session.EndPoint, out _);
                _sessionsById.TryRemove(session.SessionId, out _);
                _sessionsByUid.TryRemove(session.Uid, out _);
                Log($"Client timed out: session={session.SessionId} uid={session.Uid}");
                BroadcastClientLeft(session.SessionId, except: null);
            }
        }
    }

    private void SendAccept(ClientSession session)
    {
        Span<byte> buf = stackalloc byte[8];
        var writer = new PacketWriter(buf);
        writer.WriteByte((byte)PacketType.ConnectAccept);
        writer.WriteUInt32(session.SessionId);
        writer.WriteUInt16((ushort)_options.MaxClients);
        writer.WriteByte((byte)(_options.DebugForceAudible ? 1 : 0));
        Send(writer.Written, session.EndPoint);
    }

    private void SendReject(IPEndPoint remote, ConnectRejectReason reason)
    {
        Span<byte> buf = stackalloc byte[2];
        var writer = new PacketWriter(buf);
        writer.WriteByte((byte)PacketType.ConnectReject);
        writer.WriteByte((byte)reason);
        Send(writer.Written, remote);
    }

    private void SendClientJoined(IPEndPoint target, ClientSession subject)
    {
        Span<byte> buf = stackalloc byte[1 + 4 + 1 + Protocol.MaxUidLength + 1 + Protocol.MaxNameLength];
        var writer = new PacketWriter(buf);
        writer.WriteByte((byte)PacketType.ClientJoined);
        writer.WriteUInt32(subject.SessionId);
        writer.WriteString8(subject.Uid);
        writer.WriteString8(subject.Name);
        Send(writer.Written, target);
    }

    private void BroadcastClientJoined(ClientSession subject, uint? except)
    {
        foreach (var target in _sessionsById.Values)
        {
            if (target.SessionId == except) continue;
            if (target.SessionId == subject.SessionId) continue;
            SendClientJoined(target.EndPoint, subject);
        }
    }

    private void BroadcastClientLeft(uint sessionId, uint? except)
    {
        Span<byte> buf = stackalloc byte[5];
        var writer = new PacketWriter(buf);
        writer.WriteByte((byte)PacketType.ClientLeft);
        writer.WriteUInt32(sessionId);
        var packet = writer.Written;

        foreach (var target in _sessionsById.Values)
        {
            if (target.SessionId == except) continue;
            Send(packet, target.EndPoint);
        }
    }

    private void Send(ReadOnlySpan<byte> data, IPEndPoint target)
    {
        try
        {
            _socket.SendTo(data, SocketFlags.None, target);
        }
        catch (SocketException)
        {
            // best-effort — a single failed control-packet send should never take the server down
        }
    }

    private void FireAndForgetSend(byte[] data, IPEndPoint target)
    {
        _ = FireAndForgetSendAsync(data, target);
    }

    private async Task FireAndForgetSendAsync(byte[] data, IPEndPoint target)
    {
        try
        {
            await _socket.SendToAsync(data, SocketFlags.None, target);
        }
        catch (SocketException)
        {
            // best-effort UDP relay — ignore unreachable/reset errors from a single peer
        }
    }

    private static void Log(string message) =>
        Console.WriteLine($"[{DateTime.UtcNow:yyyy-MM-dd HH:mm:ss}Z] {message}");

    public ValueTask DisposeAsync()
    {
        _socket.Dispose();
        return ValueTask.CompletedTask;
    }
}
