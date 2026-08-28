using Tfrs.VoiceClient.Networking;

namespace Tfrs.VoiceClient.Audio;

internal enum TransmitMode
{
    PushToTalk,
    VoiceActivation,
    AlwaysOn,
}

/// <summary>
/// Ties microphone capture to the network client: applies gain, decides per-frame whether to
/// transmit (PTT / voice activation / always-on, gated by mic-mute and an optional addon override),
/// encodes with Opus, and sends. One instance for the whole app — there is only one microphone.
/// </summary>
internal sealed class TransmitController
{
    private static readonly TimeSpan VadHangover = TimeSpan.FromMilliseconds(300);

    private readonly MicCaptureService _capture;
    private readonly VoiceNetworkClient _network;
    private readonly OpusVoiceEncoder _encoder = new();
    private readonly short[] _pcmShorts = new short[OpusFormat.FrameSamples];
    private readonly byte[] _encodeBuffer = new byte[Protocol.MaxOpusFrameLength];

    private bool _isTransmitting;
    private DateTime _vadHangoverUntil = DateTime.MinValue;
    private int _silentFrameCount;
    private bool _silenceWarningRaised;

    // Raw mic sensitivity varies wildly across hardware (a cheap USB mic can sit an order of
    // magnitude quieter than a decent headset), so a single fixed default gain either clips
    // hot mics or leaves quiet ones effectively silent for VAD purposes. A small bounded AGC
    // instead adapts per-device automatically: it drives recent signal toward AgcTargetRms,
    // capped at AgcMaxGain so it can never turn room noise into a constant false trigger.
    // Attack (turning gain DOWN) is fast to protect against clipping on a sudden loud sound;
    // release (turning gain UP) is slow to avoid audibly "pumping" the noise floor between words.
    private const float AgcTargetRms = 0.08f;
    private const float AgcMinGain = 1f;
    private const float AgcMaxGain = 12f;
    private const float AgcAttackRate = 0.4f;
    private const float AgcReleaseRate = 0.05f;
    private float _autoGain = AgcMinGain;

    public TransmitMode Mode { get; set; } = TransmitMode.PushToTalk;
    public bool MicMuted { get; set; }
    public float MicGain { get; set; } = 1.0f;

    /// <summary>Linear RMS threshold for voice activation, roughly 0..0.3 in practice. Post-AGC
    /// speech typically settles well above this once the auto-gain ramps up (see AgcTargetRms
    /// above); kept lower than that target as a margin for the first ~1s before AGC catches up
    /// and for naturally quieter speakers.</summary>
    public float VadThreshold { get; set; } = 0.01f;

    /// <summary>Updated by <see cref="Hotkeys.PttKeyPoller"/>.</summary>
    public bool PttHeld { get; set; }

    /// <summary>From the addon bridge's "local" message: null = no override, true = force
    /// transmit, false = force silence (e.g. player unconscious/dead).</summary>
    public bool? AddonTransmitOverride { get; set; }

    public event Action<bool>? TransmittingChanged;
    public event Action<float>? LevelMeasured; // post-gain RMS, for a UI VU meter

    /// <summary>Fires once if the mic keeps producing exact-zero samples for a couple of
    /// seconds straight — the signature of Windows' microphone privacy gate silently zeroing a
    /// desktop app's WASAPI capture (no exception, capture "succeeds", audio just never arrives)
    /// rather than a real silent room, which almost never has literally zero noise floor. Not
    /// definitive, but worth surfacing since it's a one-setting fix
    /// (Settings → Datenschutz → Mikrofon → "Desktop-Apps auf das Mikrofon zugreifen lassen").</summary>
    public event Action? PersistentSilenceDetected;

    public TransmitController(MicCaptureService capture, VoiceNetworkClient network)
    {
        _capture = capture;
        _network = network;
        _capture.FrameCaptured += OnFrameCaptured;
    }

    private void OnFrameCaptured(float[] frame)
    {
        UpdateAutoGain(Rms(frame));
        float totalGain = MicGain * _autoGain;
        for (int i = 0; i < frame.Length; i++)
            frame[i] = Math.Clamp(frame[i] * totalGain, -1f, 1f);

        float rms = Rms(frame);
        LevelMeasured?.Invoke(rms);
        TrackSilence(rms);

        bool shouldTransmit = DetermineShouldTransmit(rms);

        if (shouldTransmit)
        {
            ToShorts(frame, _pcmShorts);
            int len = _encoder.Encode(_pcmShorts, _encodeBuffer);
            if (len > 0)
                _network.SendVoiceFrame(_encodeBuffer.AsSpan(0, len), isLast: false);
            SetTransmitting(true);
        }
        else if (_isTransmitting)
        {
            Array.Clear(_pcmShorts); // encode one frame of silence, flagged as the transmission's end
            int len = _encoder.Encode(_pcmShorts, _encodeBuffer);
            if (len > 0)
                _network.SendVoiceFrame(_encodeBuffer.AsSpan(0, len), isLast: true);
            SetTransmitting(false);
        }
    }

    private void UpdateAutoGain(float rawRms)
    {
        if (rawRms < 0.0001f) return; // don't chase pure silence/noise floor upward indefinitely
        float desired = Math.Clamp(AgcTargetRms / rawRms, AgcMinGain, AgcMaxGain);
        float rate = desired < _autoGain ? AgcAttackRate : AgcReleaseRate;
        _autoGain += (desired - _autoGain) * rate;
    }

    private bool DetermineShouldTransmit(float rms)
    {
        if (AddonTransmitOverride == false) return false;
        if (MicMuted) return false;
        if (AddonTransmitOverride == true) return true;

        return Mode switch
        {
            TransmitMode.AlwaysOn => true,
            TransmitMode.PushToTalk => PttHeld,
            TransmitMode.VoiceActivation => EvaluateVoiceActivation(rms),
            _ => false,
        };
    }

    private void TrackSilence(float rms)
    {
        if (_silenceWarningRaised) return;

        if (rms == 0f)
        {
            _silentFrameCount++;
            if (_silentFrameCount >= 150) // ~3s of 20ms frames
            {
                _silenceWarningRaised = true;
                PersistentSilenceDetected?.Invoke();
            }
        }
        else
        {
            _silentFrameCount = 0;
        }
    }

    private bool EvaluateVoiceActivation(float rms)
    {
        var now = DateTime.UtcNow;
        if (rms >= VadThreshold)
        {
            _vadHangoverUntil = now + VadHangover;
            return true;
        }
        return now < _vadHangoverUntil;
    }

    private void SetTransmitting(bool transmitting)
    {
        if (_isTransmitting == transmitting) return;
        _isTransmitting = transmitting;
        TransmittingChanged?.Invoke(transmitting);
    }

    private static float Rms(ReadOnlySpan<float> samples)
    {
        if (samples.Length == 0) return 0f;
        double sum = 0;
        foreach (float s in samples) sum += (double)s * s;
        return (float)Math.Sqrt(sum / samples.Length);
    }

    private static void ToShorts(ReadOnlySpan<float> samples, Span<short> output)
    {
        for (int i = 0; i < samples.Length; i++)
            output[i] = (short)Math.Clamp(samples[i] * 32767f, short.MinValue, short.MaxValue);
    }
}
