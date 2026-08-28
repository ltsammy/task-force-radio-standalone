namespace Tfrs.VoiceClient.Audio.Dsp;

/// <summary>Matches the "fx" field of the bridge protocol's units snapshot — see
/// docs/protocol-ipc-bridge.md and docs/dsp-audio-pipeline.md.</summary>
internal enum SourceEffect
{
    Direct,
    Sw,
    Lr,
    Airborne,
    Dd,
    Phone,
    Speaker,
    Intercom,
}

/// <summary>
/// Per-remote-source radio distortion chain, ported from the original TS3 plugin's RadioEffect.hpp
/// (see docs/dsp-audio-pipeline.md sections 3-5). One instance per currently-audible sender; holds
/// filter/delay/phase state so it must not be shared across senders or reset between frames.
///
/// Deliberate simplification vs. the original (documented in dsp-audio-pipeline.md section 8):
/// "phone" and "speaker" apply only their bandpass filter (no foldback/delay/ringmod layering with
/// the SW chain) — close enough perceptually, much simpler to keep correct without a local compiler
/// to verify against.
/// </summary>
internal sealed class RadioEffectChain
{
    private const int SampleRate = 48000;
    private const int DelaySamples = SampleRate / 20; // 50ms

    private readonly SourceEffect _effect;
    private readonly BiquadFilter? _highPass;
    private readonly BiquadFilter? _lowPass;
    private readonly CascadedFilter? _bandPass;
    private readonly float[] _delayLine = new float[DelaySamples];
    private int _delayPos;
    private float _ringPhase;
    private readonly Random _rng = new();

    public RadioEffectChain(SourceEffect effect)
    {
        _effect = effect;
        switch (effect)
        {
            case SourceEffect.Sw:
                _highPass = BiquadFilter.RbjHighPass(SampleRate, 900, 0.85);
                _lowPass = BiquadFilter.RbjLowPass(SampleRate, 3000, 2.0);
                break;
            case SourceEffect.Lr:
            case SourceEffect.Intercom:
                _highPass = BiquadFilter.RbjHighPass(SampleRate, 520, 0.97);
                _lowPass = BiquadFilter.RbjLowPass(SampleRate, 1300, 1.0);
                break;
            case SourceEffect.Airborne:
                _highPass = BiquadFilter.RbjHighPass(SampleRate, 1000, 1.0);
                _lowPass = BiquadFilter.RbjLowPass(SampleRate, 4000, 1.0);
                break;
            case SourceEffect.Dd:
                _bandPass = CascadedFilter.BandPass(SampleRate, 1000, 400, order: 2);
                break;
            case SourceEffect.Phone:
                _bandPass = CascadedFilter.BandPass(SampleRate, 1850, 1550, order: 2);
                break;
            case SourceEffect.Speaker:
                _bandPass = CascadedFilter.BandPass(SampleRate, 2000, 1000, order: 1);
                break;
            case SourceEffect.Direct:
                break; // no radio distortion at all — just distance gain + panning
        }
    }

    /// <param name="errorLevel">0..1, from the extension's `err` field — see
    /// docs/dsp-audio-pipeline.md section 1/6.</param>
    public void Process(Span<float> buffer, float errorLevel)
    {
        switch (_effect)
        {
            case SourceEffect.Direct:
                return;

            case SourceEffect.Dd:
                ProcessDiverRadio(buffer, errorLevel);
                return;

            case SourceEffect.Phone:
            case SourceEffect.Speaker:
                _bandPass?.Process(buffer);
                return;

            default:
                ProcessDistortedRadio(buffer, errorLevel);
                return;
        }
    }

    private void ProcessDistortedRadio(Span<float> buffer, float rawErrorLevel)
    {
        float errorLevel = CalcErrorLevel(rawErrorLevel);

        float sum = 0f;
        for (int i = 0; i < buffer.Length; i++) sum += MathF.Abs(buffer[i]);
        float avg = buffer.Length > 0 ? sum / buffer.Length : 0f;
        float threshold = 0.3f * (1f - errorLevel) * (avg / 0.005f);

        for (int i = 0; i < buffer.Length; i++)
            buffer[i] = Foldback(buffer[i], threshold);

        for (int i = 0; i < buffer.Length; i++)
            buffer[i] = RingModulation(Delay(buffer[i] * 30f), errorLevel);

        _highPass?.Process(buffer);
        _lowPass?.Process(buffer);
    }

    private void ProcessDiverRadio(Span<float> buffer, float errorLevel)
    {
        for (int i = 0; i < buffer.Length; i++)
        {
            if (_rng.NextDouble() < errorLevel) buffer[i] = 0f;
        }
        _bandPass?.Process(buffer);
        for (int i = 0; i < buffer.Length; i++) buffer[i] *= 30f;
    }

    private float Delay(float input)
    {
        _delayLine[_delayPos] = input;
        _delayPos = (_delayPos + 1) % DelaySamples;
        return _delayLine[_delayPos];
    }

    private float RingModulation(float input, float mix)
    {
        float modulated = input * MathF.Sin(_ringPhase * MathF.PI / 2f);
        _ringPhase += 90.0f / SampleRate;
        if (_ringPhase > 1.0f) _ringPhase = 0f;
        return input * (1f - mix) + modulated * mix;
    }

    /// <summary>C#'s `%` on floats matches C's truncated fmod — same semantics as the original.</summary>
    private static float Foldback(float input, float threshold)
    {
        if (threshold < 0.00001f) return 0f;
        if (input > threshold || input < -threshold)
            input = MathF.Abs(MathF.Abs((input - threshold) % (threshold * 4f)) - threshold * 2f) - threshold;
        return input;
    }

    private static readonly float[] ErrorLevels =
    {
        0f, 0.150000006f, 0.300000012f, 0.600000024f, 0.899999976f, 0.950000048f,
        0.960000038f, 0.970000029f, 0.980000019f, 0.995000005f, 0.997799993f,
        0.998799993f, 0.99999f,
    };

    private static float CalcErrorLevel(float errorLevel)
    {
        int part = Math.Clamp((int)(errorLevel * 10f), 0, ErrorLevels.Length - 2);
        float from = ErrorLevels[part], to = ErrorLevels[part + 1];
        return from + (to - from) * (errorLevel - part / 10f);
    }
}
