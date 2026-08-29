using Tfrs.VoiceClient.Audio;
using Tfrs.VoiceClient.Audio.Dsp;
using Tfrs.VoiceClient.Bridge;
using Tfrs.VoiceClient.Networking;
using Tfrs.VoiceClient.Settings;

namespace Tfrs.VoiceClient;

/// <summary>
/// Wires the network client, audio pipeline, and addon bridge together. Owns all of the app's
/// background state; MainWindow only translates UI actions into calls here and mirrors the events
/// back onto the UI thread. Every event below fires on a background thread (network I/O, the mic
/// capture callback, or the bridge pipe thread) — subscribers must marshal to the UI thread
/// themselves before touching WPF elements.
/// </summary>
internal sealed class VoiceSessionCoordinator : IAsyncDisposable
{
    private readonly VoiceNetworkClient _network = new();
    private readonly MicCaptureService _micCapture = new();
    private readonly PlaybackEngine _playback = new();
    private readonly TransmitController _transmit;
    private readonly AddonBridgeServer _bridge = new();
    private readonly System.Threading.Timer _statusHeartbeat;
    private readonly StaWorker _audioThread = new();

    private readonly Dictionary<uint, string> _sessionToUid = new();
    private readonly Dictionary<string, uint> _uidToSession = new();
    private readonly HashSet<string> _lastActiveUnitUids = new();
    private readonly object _rosterLock = new();

    // Remote radio-transmit state relayed via the voice server (see docs/protocol-ipc-bridge.md
    // "localTx"/"tx") -- longer than the extension's own 1.5s kTxExpiry so our snapshot never goes
    // stale before the extension's independent expiry would anyway; this is just a safety net for
    // a sender that disconnects without an explicit "stopped" update.
    private static readonly TimeSpan RemoteTxExpiry = TimeSpan.FromSeconds(2);
    private readonly Dictionary<uint, (string Freq, int Range, string Sub, DateTime ExpiresUtc)> _remoteTx = new();

    private bool _isTransmitting;

    public AppSettings Settings { get; }

    public event Action<bool>? ConnectionStateChanged;
    public event Action<string>? ConnectError;
    public event Action<bool>? TransmittingChanged;
    public event Action<float>? MicLevelMeasured;
    public event Action<float>? SpeakerLevelMeasured;
    public event Action? MicPersistentSilence;
    public event Action? MicDeviceMuted;
    public event Action<bool>? ExtensionConnectionChanged;

    /// <summary>Total clients currently on the voice server, including this one. No names/roster
    /// UI by design — just a headcount (see project notes: no display names, no "who's talking").</summary>
    public event Action<int>? ConnectedCountChanged;

    /// <summary>Fires whenever the Arma extension reports (or corrects) the local player's real
    /// UID (see AddonBridgeServer.LocalUidReceived) and it differs from what Settings.LocalUid
    /// already held — Settings.LocalUid has already been updated by the time this fires.
    /// Requiring a player to manually type/pass their own Steam ID is exactly the fragile setup
    /// that caused today's "some players just aren't heard" bug (see the relay log showing
    /// MachineName:Username fallback UIDs) — the addon always knows this authoritatively via
    /// getPlayerUID, so it should always win once available. MainWindow reacts to this by
    /// (re)connecting under the corrected identity.</summary>
    public event Action? AddonUidUpdated;

    /// <summary>The addon's TFAR_ADDON_VERSION, once known — see AddonBridgeServer.AddonVersionReceived.
    /// Purely diagnostic (logged), to catch a client/server/addon version mismatch quickly.</summary>
    public event Action<string>? AddonVersionReceived;

    /// <summary>A bad/unexpected message from the extension, or an unexpected failure in the bridge
    /// read loop — see AddonBridgeServer.BridgeError. Always already recovered from by the time
    /// this fires; purely diagnostic.</summary>
    public event Action<string>? BridgeError;

    /// <summary>The Arma extension's transmitOverride, forwarded for logging/diagnostics — see
    /// docs/protocol-ipc-bridge.md's "local" message. `false` blocks ALL transmission (including
    /// Always-On); `true` forces it regardless of mode; `null` clears the override.</summary>
    public event Action<bool?>? AddonOverrideChanged;

    /// <summary>Fires only for a connection loss the watchdog detected on its own (server
    /// crashed/restarted/network cut) — never for a user-initiated Disconnect. MainWindow uses
    /// this to retry automatically instead of leaving the player stuck on a dead connection until
    /// they notice and click Connect again.</summary>
    public event Action? ConnectionLostUnexpectedly;

    public VoiceSessionCoordinator(AppSettings settings)
    {
        Settings = settings;
        _transmit = new TransmitController(_micCapture, _network)
        {
            Mode = settings.TransmitMode,
            MicGain = settings.MicVolume,
            VadThreshold = settings.VadThreshold,
        };
        _playback.MasterVolume = settings.SpeakerVolume;

        _network.Connected += () => { ConnectionStateChanged?.Invoke(true); PushConnectedCount(); };
        _network.ConnectFailed += msg => ConnectError?.Invoke(msg);
        // Fires for BOTH a user-initiated disconnect and one the network layer detected on its own
        // (server timeout/crash — see VoiceNetworkClient.WatchdogLoopAsync) — either way, local
        // state (mic, playback devices, per-session sources, roster) must be torn down the same
        // way, or a reconnect can inherit stale RemoteVoiceSource objects/state from before.
        _network.Disconnected += cause =>
        {
            ConnectionStateChanged?.Invoke(false);
            _ = TeardownLocalStateAsync();
            if (cause == DisconnectCause.ConnectionLost) ConnectionLostUnexpectedly?.Invoke();
        };
        _network.VoiceFrameReceived += OnVoiceFrameReceived;
        _network.RemoteJoined += OnRemoteJoined;
        _network.RemoteLeft += OnRemoteLeft;
        // Must land before any ClientJoined the server sends right after ConnectAccept can create
        // a source — see the doc comment on DebugFlagReceived for why the awaited ConnectAsync
        // return is too late for this.
        _network.DebugFlagReceived += audible => _playback.DebugForceAudible = audible;
        _network.RadioTxReceived += OnRadioTxReceived;

        _transmit.TransmittingChanged += t => { _isTransmitting = t; TransmittingChanged?.Invoke(t); };
        _transmit.LevelMeasured += l => MicLevelMeasured?.Invoke(l);
        _transmit.PersistentSilenceDetected += () => MicPersistentSilence?.Invoke();
        _micCapture.DeviceMuted += () => MicDeviceMuted?.Invoke();
        _playback.LevelMeasured += l => SpeakerLevelMeasured?.Invoke(l);

        _bridge.UnitsReceived += OnUnitsReceived;
        // AddonTransmitOverride == false silently blocks transmission entirely, including
        // Always-On -- this was previously invisible from the outside, making "always-on doesn't
        // transmit" undiagnosable without a debugger. Surfaced so it shows up in the log instead.
        _bridge.LocalOverrideReceived += v => { _transmit.AddonTransmitOverride = v; AddonOverrideChanged?.Invoke(v); };
        _bridge.LocalUidReceived += OnLocalUidReceived;
        _bridge.AddonVersionReceived += v => AddonVersionReceived?.Invoke(v);
        _bridge.BridgeError += msg => BridgeError?.Invoke(msg);
        _bridge.LocalTxChanged += state => _network.SendRadioTx(
            state.Active, state.Freq, (ushort)Math.Clamp(state.Range, 0, ushort.MaxValue), state.Sub);
        _bridge.ExtensionConnected += () => ExtensionConnectionChanged?.Invoke(true);
        // Without resetting this, a stale AddonTransmitOverride==false from just before the
        // extension went away would stay stuck forever, silently blocking ALL transmission
        // (including Always-On) even with no addon connected at all -- normal PTT/VAD/Always-On
        // must apply again the moment we know the extension is actually gone.
        _bridge.ExtensionDisconnected += () =>
        {
            ExtensionConnectionChanged?.Invoke(false);
            _transmit.AddonTransmitOverride = null;
            AddonOverrideChanged?.Invoke(null);
        };

        _statusHeartbeat = new System.Threading.Timer(_ => { PushStatus(); PruneExpiredTx(); }, null, TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(1));
    }

    public bool IsConnected => _network.IsConnected;

    /// <summary>Mirrors the server's ConnectAccept flag — see ServerOptions.DebugForceAudible.
    /// Never settable from here; the client has no local override.</summary>
    public bool ServerDebugForceAudible => _network.ServerDebugForceAudible;
    public string ServerVersion => _network.ServerVersion;
    public bool MicMuted { get => _transmit.MicMuted; set { _transmit.MicMuted = value; PushStatus(); } }
    public bool SpeakerMuted { get => _playback.SpeakerMuted; set { _playback.SpeakerMuted = value; PushStatus(); } }

    public TransmitMode TransmitMode
    {
        get => _transmit.Mode;
        set { _transmit.Mode = value; Settings.TransmitMode = value; Settings.Save(); }
    }

    public float MicVolume
    {
        get => _transmit.MicGain;
        set { _transmit.MicGain = value; Settings.MicVolume = value; Settings.Save(); }
    }

    public float SpeakerVolume
    {
        get => _playback.MasterVolume;
        set { _playback.MasterVolume = value; Settings.SpeakerVolume = value; Settings.Save(); }
    }

    public float VadThreshold
    {
        get => _transmit.VadThreshold;
        set { _transmit.VadThreshold = value; Settings.VadThreshold = value; Settings.Save(); }
    }

    public void SetPttHeld(bool held) => _transmit.PttHeld = held;

    public void SetInputDevice(string? deviceId)
    {
        Settings.InputDeviceId = deviceId;
        if (!IsConnected) return;
        // WASAPI device resolution AND activation are COM calls that need an STA apartment (see
        // StaWorker) — and both need to run on the *same* STA thread: an MMDevice resolved on one
        // STA thread (e.g. the UI thread) is a COM proxy tied to that apartment, and activating it
        // from a different STA thread (a plain Task.Run is MTA and fails even harder) throws
        // InvalidCastException/E_NOINTERFACE despite looking like a valid object reference.
        _ = _audioThread.InvokeAsync(() =>
        {
            var device = AudioDevices.Resolve(deviceId, NAudio.CoreAudioApi.DataFlow.Capture);
            if (device is not null) _micCapture.Start(device);
        });
    }

    public void SetOutputDevice(string? deviceId)
    {
        Settings.OutputDeviceId = deviceId;
        _ = _audioThread.InvokeAsync(() =>
        {
            var device = AudioDevices.Resolve(deviceId, NAudio.CoreAudioApi.DataFlow.Render);
            if (device is not null) _playback.Start(device);
        });
    }

    public async Task<bool> ConnectAsync(string host, int port, string password, string displayName)
    {
        string uid = string.IsNullOrWhiteSpace(Settings.LocalUid) ? GetFallbackUid() : Settings.LocalUid;
        bool ok = await _network.ConnectAsync(host, port, password, uid, displayName, CancellationToken.None);
        if (!ok) return false;

        // Same reasoning as SetInputDevice/SetOutputDevice above — resolve AND activate on the
        // same STA thread. This used to run inline here on the UI thread and froze the window for
        // as long as WASAPI setup took; then briefly ran on a second STA thread while still
        // resolving devices on the caller's (UI) thread, which crashed activation outright.
        await _audioThread.InvokeAsync(() =>
        {
            var inputDevice = AudioDevices.Resolve(Settings.InputDeviceId, NAudio.CoreAudioApi.DataFlow.Capture);
            if (inputDevice is not null) _micCapture.Start(inputDevice);

            var outputDevice = AudioDevices.Resolve(Settings.OutputDeviceId, NAudio.CoreAudioApi.DataFlow.Render);
            if (outputDevice is not null) _playback.Start(outputDevice);
        });

        return true;
    }

    public async Task DisconnectAsync()
    {
        // _network.DisconnectAsync() also fires the Disconnected event (see the constructor
        // wiring), which independently kicks off TeardownLocalStateAsync — a harmless redundant
        // run (every step it does is idempotent). Awaiting it explicitly here too is what actually
        // matters: it guarantees mic/playback are fully stopped and sources cleared BEFORE this
        // method returns, so an immediate reconnect right after can't race a still-in-progress
        // teardown (that race is what used to leave the mic not actually (re)capturing after a
        // quick disconnect/reconnect).
        await _network.DisconnectAsync();
        await TeardownLocalStateAsync();
    }

    private async Task TeardownLocalStateAsync()
    {
        await _audioThread.InvokeAsync(() =>
        {
            _micCapture.Stop();
            _playback.Stop();
        });
        _playback.RemoveAllSources();
        lock (_rosterLock)
        {
            _sessionToUid.Clear();
            _uidToSession.Clear();
            _lastActiveUnitUids.Clear();
            _remoteTx.Clear();
        }
        ConnectedCountChanged?.Invoke(0);
    }

    /// <summary>Used only until the Arma extension reports the real UID on its own (see
    /// OnLocalUidReceived) — e.g. the very first few seconds before a mission has loaded, or
    /// standalone testing with no Arma running at all (the debug-force-audible flow). Never meant
    /// to be a real deployment identity: two different machines/users can collide, which is
    /// harmless as a placeholder but would silently misroute audio if it stuck around.</summary>
    private static string GetFallbackUid() =>
        Environment.MachineName + ":" + Environment.UserName;

    /// <summary>The addon always knows the local player's real Steam UID authoritatively
    /// (getPlayerUID) — once it reports one, it must win over whatever this connected with
    /// before (a CLI --uid, a stale persisted Settings.LocalUid, or the machine-name fallback).
    /// Requiring correct manual UID entry is exactly the fragile setup that silently broke voice
    /// for two testers in practice (see the relay log showing MachineName:Username UIDs).</summary>
    private void OnLocalUidReceived(string uid)
    {
        if (Settings.LocalUid == uid) return;
        Settings.LocalUid = uid;
        Settings.Save();
        AddonUidUpdated?.Invoke();
    }

    private void OnVoiceFrameReceived(uint senderSessionId, ushort sequence, bool isLast, byte[] opus)
    {
        string uid;
        lock (_rosterLock)
        {
            if (!_sessionToUid.TryGetValue(senderSessionId, out uid!))
                uid = $"unknown:{senderSessionId}";
        }
        var source = _playback.GetOrAddSource(senderSessionId, uid);
        source.EnqueueOpusFrame(opus);
    }

    private void OnRemoteJoined(uint sessionId, string uid, string name)
    {
        lock (_rosterLock)
        {
            _sessionToUid[sessionId] = uid;
            _uidToSession[uid] = sessionId;
        }
        _playback.GetOrAddSource(sessionId, uid);
        PushConnectedCount();
    }

    private void OnRemoteLeft(uint sessionId)
    {
        bool hadTx;
        lock (_rosterLock)
        {
            if (_sessionToUid.Remove(sessionId, out var uid))
                _uidToSession.Remove(uid);
            hadTx = _remoteTx.Remove(sessionId);
        }
        _playback.RemoveSource(sessionId);
        PushConnectedCount();
        if (hadTx) PushTxToExtension();
    }

    /// <summary>Another client relayed what it's currently transmitting on radio (or that it
    /// stopped) — see VoiceNetworkClient.RadioTxReceived. Full snapshot semantics: this just
    /// updates our view of that one sender and re-pushes the whole current set to our own
    /// extension, same pattern as "units".</summary>
    private void OnRadioTxReceived(uint senderSessionId, bool active, string freq, ushort range, string sub)
    {
        lock (_rosterLock)
        {
            if (active)
                _remoteTx[senderSessionId] = (freq, range, sub, DateTime.UtcNow + RemoteTxExpiry);
            else
                _remoteTx.Remove(senderSessionId);
        }
        PushTxToExtension();
    }

    private void PruneExpiredTx()
    {
        bool changed = false;
        lock (_rosterLock)
        {
            var now = DateTime.UtcNow;
            var expired = new List<uint>();
            foreach (var kv in _remoteTx)
                if (kv.Value.ExpiresUtc < now) expired.Add(kv.Key);
            foreach (var key in expired) { _remoteTx.Remove(key); changed = true; }
        }
        if (changed) PushTxToExtension();
    }

    private void PushTxToExtension()
    {
        var entries = new List<RemoteTxEntry>();
        lock (_rosterLock)
        {
            foreach (var kv in _remoteTx)
            {
                if (!_sessionToUid.TryGetValue(kv.Key, out var uid)) continue;
                entries.Add(new RemoteTxEntry(uid, kv.Value.Freq, kv.Value.Range, kv.Value.Sub));
            }
        }
        _ = _bridge.SendTxAsync(entries);
    }

    private void PushConnectedCount()
    {
        int remoteCount;
        lock (_rosterLock) remoteCount = _sessionToUid.Count;
        ConnectedCountChanged?.Invoke(remoteCount + (_network.IsConnected ? 1 : 0));
    }

    private void OnUnitsReceived(IReadOnlyList<UnitEntry> units)
    {
        // Server dictated debug/test mode: every source was already forced audible when created
        // (PlaybackEngine.DebugForceAudible) — ignore whatever the extension computes so it can't
        // silence/pan anyone back, keeping this a clean test of raw voice transport.
        if (ServerDebugForceAudible) return;

        var seenUids = new HashSet<string>(units.Count);

        foreach (var unit in units)
        {
            seenUids.Add(unit.Uid);
            uint sessionId;
            lock (_rosterLock)
            {
                if (!_uidToSession.TryGetValue(unit.Uid, out sessionId))
                    continue; // not on the voice server (yet) — nothing to route this to
            }

            if (_playback.TryGetSource(sessionId, out var source))
            {
                var effect = ParseEffect(unit.Fx);
                source.SetState(new RemoteSourceState(unit.Gain, unit.Azimuth, unit.Muted, effect, unit.ErrorLevel));
            }
        }

        // Anything previously audible but missing from this snapshot goes silent — the bridge
        // protocol defines "units" as a full replace, not a delta (docs/protocol-ipc-bridge.md).
        foreach (string uid in _lastActiveUnitUids)
        {
            if (seenUids.Contains(uid)) continue;
            uint sessionId;
            lock (_rosterLock)
            {
                if (!_uidToSession.TryGetValue(uid, out sessionId)) continue;
            }
            if (_playback.TryGetSource(sessionId, out var source))
                source.SetState(RemoteSourceState.Silent);
        }

        _lastActiveUnitUids.Clear();
        _lastActiveUnitUids.UnionWith(seenUids);
    }

    private static SourceEffect ParseEffect(string fx) => fx switch
    {
        "sw" => SourceEffect.Sw,
        "lr" => SourceEffect.Lr,
        "airborne" => SourceEffect.Airborne,
        "dd" => SourceEffect.Dd,
        "phone" => SourceEffect.Phone,
        "speaker" => SourceEffect.Speaker,
        "intercom" => SourceEffect.Intercom,
        _ => SourceEffect.Direct,
    };

    private void PushStatus()
    {
        _ = _bridge.SendStatusAsync(_network.IsConnected, _network.IsConnected ? _network.SessionId : null,
            _transmit.MicMuted, _playback.SpeakerMuted, _isTransmitting, error: null);
    }

    public async ValueTask DisposeAsync()
    {
        _statusHeartbeat.Dispose();
        await _audioThread.InvokeAsync(() =>
        {
            _micCapture.Dispose();
            _playback.Dispose();
        });
        _audioThread.Dispose();
        await _network.DisposeAsync();
        await _bridge.DisposeAsync();
    }
}
