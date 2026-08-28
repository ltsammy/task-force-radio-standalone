using Concentus;
using Concentus.Enums;

namespace Tfrs.VoiceClient.Audio;

internal static class OpusFormat
{
    public const int SampleRate = 48000;
    public const int Channels = 1;
    public const int FrameMillis = 20;
    public const int FrameSamples = SampleRate * FrameMillis / 1000; // 960
    public const int Bitrate = 24000;
}

internal sealed class OpusVoiceEncoder
{
    private readonly IOpusEncoder _encoder;

    public OpusVoiceEncoder()
    {
        _encoder = OpusCodecFactory.CreateEncoder(OpusFormat.SampleRate, OpusFormat.Channels, OpusApplication.OPUS_APPLICATION_VOIP);
        _encoder.Bitrate = OpusFormat.Bitrate;
        _encoder.Complexity = 6;
        _encoder.UseVBR = true;
    }

    /// <summary>Encodes exactly one 20ms frame (<see cref="OpusFormat.FrameSamples"/> samples).</summary>
    public int Encode(ReadOnlySpan<short> pcm, Span<byte> output) =>
        _encoder.Encode(pcm, OpusFormat.FrameSamples, output, output.Length);
}

internal sealed class OpusVoiceDecoder
{
    private readonly IOpusDecoder _decoder = OpusCodecFactory.CreateDecoder(OpusFormat.SampleRate, OpusFormat.Channels);

    public int Decode(ReadOnlySpan<byte> opusPayload, Span<short> outputPcm) =>
        _decoder.Decode(opusPayload, outputPcm, OpusFormat.FrameSamples, false);

    /// <summary>Packet-loss concealment: synthesizes one frame without new input data.</summary>
    public int DecodePacketLoss(Span<short> outputPcm) =>
        _decoder.Decode(ReadOnlySpan<byte>.Empty, outputPcm, OpusFormat.FrameSamples, false);
}
