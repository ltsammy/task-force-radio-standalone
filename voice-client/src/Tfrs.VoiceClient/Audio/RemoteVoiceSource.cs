using System.Collections.Concurrent;
using NAudio.Wave;
using Tfrs.VoiceClient.Audio.Dsp;

namespace Tfrs.VoiceClient.Audio;

internal sealed record RemoteSourceState(float Gain, float AzimuthRadians, bool Muted, SourceEffect Effect, float ErrorLevel)
{
    public static readonly RemoteSourceState Silent = new(0f, 0f, true, SourceEffect.Direct, 0f);
}

/// <summary>
/// Pull-based per-sender playback: decodes queued Opus frames (jitter-buffered), applies the
/// radio-effect chain + panning driven by the latest bridge-supplied <see cref="RemoteSourceState"/>,
/// and exposes itself as a stereo <see cref="ISampleProvider"/> for NAudio's MixingSampleProvider.
/// One instance per currently-known remote session; created/destroyed with roster membership.
/// </summary>
internal sealed class RemoteVoiceSource : ISampleProvider
{
    private const int JitterTargetFrames = 2; // ~40ms buffered before playback starts
    private const int MaxConcealmentFrames = 5; // ~100ms of PLC before going silent

    private readonly ConcurrentQueue<byte[]> _pending = new();
    private readonly OpusVoiceDecoder _decoder = new();
    private readonly short[] _decodedShorts = new short[OpusFormat.FrameSamples];
    private readonly float[] _stereoFrame = new float[OpusFormat.FrameSamples * 2];
    private int _stereoFramePos = OpusFormat.FrameSamples * 2; // force a decode on first Read
    private bool _isPlaying;
    private int _concealmentCount;
    private RadioEffectChain _effectChain = new(SourceEffect.Direct);
    private SourceEffect _currentEffect = SourceEffect.Direct;

    private volatile RemoteSourceState _state = RemoteSourceState.Silent;

    public uint SessionId { get; }
    public string Uid { get; }

    public WaveFormat WaveFormat { get; } = WaveFormat.CreateIeeeFloatWaveFormat(OpusFormat.SampleRate, 2);

    public RemoteVoiceSource(uint sessionId, string uid)
    {
        SessionId = sessionId;
        Uid = uid;
    }

    public void SetState(RemoteSourceState state) => _state = state;

    /// <summary>Called from the network receive thread as VoiceDown packets arrive.</summary>
    public void EnqueueOpusFrame(byte[] opusPayload)
    {
        _pending.Enqueue(opusPayload);
        // Bound the queue so a stalled playback thread can't accumulate unbounded latency.
        while (_pending.Count > 10 && _pending.TryDequeue(out _)) { }
    }

    public int Read(float[] buffer, int offset, int count)
    {
        var state = _state;
        int written = 0;
        while (written < count)
        {
            if (_stereoFramePos >= _stereoFrame.Length)
            {
                if (!TryProduceNextFrame(state))
                {
                    // Nothing to play right now — emit silence for the remainder of this callback.
                    Array.Clear(buffer, offset + written, count - written);
                    return count;
                }
                _stereoFramePos = 0;
            }

            int available = _stereoFrame.Length - _stereoFramePos;
            int toCopy = Math.Min(available, count - written);
            Array.Copy(_stereoFrame, _stereoFramePos, buffer, offset + written, toCopy);
            _stereoFramePos += toCopy;
            written += toCopy;
        }
        return written;
    }

    private bool TryProduceNextFrame(RemoteSourceState state)
    {
        if (state.Muted)
        {
            _isPlaying = false;
            return _pending.IsEmpty; // drain quietly rather than leaving stale packets queued
        }

        bool hasPacket = _pending.TryDequeue(out var opus);

        if (!hasPacket)
        {
            if (!_isPlaying) return false; // nothing was playing — stay silent, don't fabricate audio
            if (_concealmentCount >= MaxConcealmentFrames)
            {
                _isPlaying = false;
                _concealmentCount = 0;
                return false;
            }
            _concealmentCount++;
            _decoder.DecodePacketLoss(_decodedShorts);
        }
        else
        {
            if (!_isPlaying)
            {
                // (Re)starting a transmission: wait for a short jitter cushion before audio begins,
                // to smooth out network jitter on the first few frames of a PTT press.
                if (_pending.Count < JitterTargetFrames - 1)
                {
                    _pending.Enqueue(opus!); // put it back; not enough buffered yet
                    return false;
                }
                _isPlaying = true;
            }
            _concealmentCount = 0;
            _decoder.Decode(opus!, _decodedShorts);
        }

        EnsureEffectChain(state.Effect);
        ApplyEffectAndPan(state);
        return true;
    }

    private void EnsureEffectChain(SourceEffect effect)
    {
        if (effect == _currentEffect) return;
        _currentEffect = effect;
        _effectChain = new RadioEffectChain(effect);
    }

    private void ApplyEffectAndPan(RemoteSourceState state)
    {
        Span<float> mono = stackalloc float[OpusFormat.FrameSamples];
        for (int i = 0; i < mono.Length; i++)
            mono[i] = _decodedShorts[i] / 32768f;

        _effectChain.Process(mono, state.ErrorLevel);

        var (left, right) = Panning.Compute(state.AzimuthRadians);
        left *= state.Gain;
        right *= state.Gain;

        for (int i = 0; i < mono.Length; i++)
        {
            _stereoFrame[i * 2] = Math.Clamp(mono[i] * left, -1f, 1f);
            _stereoFrame[i * 2 + 1] = Math.Clamp(mono[i] * right, -1f, 1f);
        }
    }
}
