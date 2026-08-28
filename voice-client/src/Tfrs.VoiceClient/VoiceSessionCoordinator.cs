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

    private readonly Dictionary<uint, string> _sessionToUid = new();
    private readonly Dictionary<string, uint> _uidToSession = new();
    private readonly HashSet<string> _lastActiveUnitUids = new();
    private readonly object _rosterLock = new();

    private bool _isTransmitting;

    public AppSettings Settings { get; }

    public event Action<bool>? ConnectionStateChanged;
    public event Action<string>? ConnectError;
    public event Action<bool>? TransmittingChanged;
    public event Action<float>? MicLevelMeasured;
    public event Action<bool>? ExtensionConnectionChanged;

    /// <summary>Total clients currently on the voice server, including this one. No names/roster
    /// UI by design — just a headcount (see project notes: no display names, no "who's talking").</summary>
    public event Action<int>? ConnectedCountChanged;

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
        _network.Disconnected += _ => { ConnectionStateChanged?.Invoke(false); ConnectedCountChanged?.Invoke(0); };
        _network.VoiceFrameReceived += OnVoiceFrameReceived;
        _network.RemoteJoined += OnRemoteJoined;
        _network.RemoteLeft += OnRemoteLeft;

        _transmit.TransmittingChanged += t => { _isTransmitting = t; TransmittingChanged?.Invoke(t); };
        _transmit.LevelMeasured += l => MicLevelMeasured?.Invoke(l);

        _bridge.UnitsReceived += OnUnitsReceived;
        _bridge.LocalOverrideReceived += v => _transmit.AddonTransmitOverride = v;
        _bridge.ExtensionConnected += () => ExtensionConnectionChanged?.Invoke(true);
        _bridge.ExtensionDisconnected += () => ExtensionConnectionChanged?.Invoke(false);

        _statusHeartbeat = new System.Threading.Timer(_ => PushStatus(), null, TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(1));
    }

    public bool IsConnected => _network.IsConnected;
    public bool MicMuted { get => _transmit.MicMuted; set { _transmit.MicMuted = value; PushStatus(); } }
    public bool SpeakerMuted { get => _playback.SpeakerMuted; set { _playback.SpeakerMuted = value; PushStatus(); } }

    public TransmitMode TransmitMode
    {
        get => _transmit.Mode;
        set { _transmit.Mode = value; Settings.TransmitMode = value; }
    }

    public float MicVolume
    {
        get => _transmit.MicGain;
        set { _transmit.MicGain = value; Settings.MicVolume = value; }
    }

    public float SpeakerVolume
    {
        get => _playback.MasterVolume;
        set { _playback.MasterVolume = value; Settings.SpeakerVolume = value; }
    }

    public float VadThreshold
    {
        get => _transmit.VadThreshold;
        set { _transmit.VadThreshold = value; Settings.VadThreshold = value; }
    }

    public void SetPttHeld(bool held) => _transmit.PttHeld = held;

    public void SetInputDevice(string? deviceId)
    {
        Settings.InputDeviceId = deviceId;
        if (!IsConnected) return;
        var device = AudioDevices.Resolve(deviceId, NAudio.CoreAudioApi.DataFlow.Capture);
        if (device is not null) _micCapture.Start(device);
    }

    public void SetOutputDevice(string? deviceId)
    {
        Settings.OutputDeviceId = deviceId;
        var device = AudioDevices.Resolve(deviceId, NAudio.CoreAudioApi.DataFlow.Render);
        if (device is not null) _playback.Start(device);
    }

    public async Task<bool> ConnectAsync(string host, int port, string password, string displayName)
    {
        string uid = string.IsNullOrWhiteSpace(Settings.LocalUid) ? GetFallbackUid() : Settings.LocalUid;
        bool ok = await _network.ConnectAsync(host, port, password, uid, displayName, CancellationToken.None);
        if (!ok) return false;

        var inputDevice = AudioDevices.Resolve(Settings.InputDeviceId, NAudio.CoreAudioApi.DataFlow.Capture);
        if (inputDevice is not null) _micCapture.Start(inputDevice);

        var outputDevice = AudioDevices.Resolve(Settings.OutputDeviceId, NAudio.CoreAudioApi.DataFlow.Render);
        if (outputDevice is not null) _playback.Start(outputDevice);

        return true;
    }

    public async Task DisconnectAsync()
    {
        _micCapture.Stop();
        _playback.Stop();
        await _network.DisconnectAsync();
        lock (_rosterLock)
        {
            _sessionToUid.Clear();
            _uidToSession.Clear();
            _lastActiveUnitUids.Clear();
        }
        ConnectedCountChanged?.Invoke(0);
    }

    /// <summary>Used only when no real Arma UID was supplied (Settings.LocalUid, normally set via
    /// the `--uid` startparameter — see CommandLineArgs). Without a matching UID the bridge's
    /// "units" messages from the extension can never resolve to this connection, so this is a
    /// standalone-testing fallback, not a real deployment path.</summary>
    private static string GetFallbackUid() =>
        Environment.MachineName + ":" + Environment.UserName;

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
        lock (_rosterLock)
        {
            if (_sessionToUid.Remove(sessionId, out var uid))
                _uidToSession.Remove(uid);
        }
        _playback.RemoveSource(sessionId);
        PushConnectedCount();
    }

    private void PushConnectedCount()
    {
        int remoteCount;
        lock (_rosterLock) remoteCount = _sessionToUid.Count;
        ConnectedCountChanged?.Invoke(remoteCount + (_network.IsConnected ? 1 : 0));
    }

    private void OnUnitsReceived(IReadOnlyList<UnitEntry> units)
    {
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
        _micCapture.Dispose();
        _playback.Dispose();
        await _network.DisposeAsync();
        await _bridge.DisposeAsync();
    }
}
