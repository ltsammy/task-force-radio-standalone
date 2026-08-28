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

    public TransmitMode Mode { get; set; } = TransmitMode.PushToTalk;
    public bool MicMuted { get; set; }
    public float MicGain { get; set; } = 1.0f;

    /// <summary>Linear RMS threshold for voice activation, roughly 0..0.3 in practice.</summary>
    public float VadThreshold { get; set; } = 0.02f;

    /// <summary>Updated by <see cref="Hotkeys.PttKeyPoller"/>.</summary>
    public bool PttHeld { get; set; }

    /// <summary>From the addon bridge's "local" message: null = no override, true = force
    /// transmit, false = force silence (e.g. player unconscious/dead).</summary>
    public bool? AddonTransmitOverride { get; set; }

    public event Action<bool>? TransmittingChanged;
    public event Action<float>? LevelMeasured; // post-gain RMS, for a UI VU meter

    public TransmitController(MicCaptureService capture, VoiceNetworkClient network)
    {
        _capture = capture;
        _network = network;
        _capture.FrameCaptured += OnFrameCaptured;
    }

    private void OnFrameCaptured(float[] frame)
    {
        for (int i = 0; i < frame.Length; i++)
            frame[i] = Math.Clamp(frame[i] * MicGain, -1f, 1f);

        float rms = Rms(frame);
        LevelMeasured?.Invoke(rms);

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
