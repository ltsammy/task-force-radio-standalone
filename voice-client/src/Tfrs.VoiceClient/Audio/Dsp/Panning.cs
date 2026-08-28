namespace Tfrs.VoiceClient.Audio.Dsp;

/// <summary>
/// Simplified stereo panning (docs/dsp-audio-pipeline.md section 2) — a deliberate stand-in for the
/// original's Clunk/KEMAR HRTF convolution, using the same cosine gain law TFAR itself used for
/// speaker radios. Good enough directional cue for a stereo setup without a binaural convolution
/// engine.
/// </summary>
internal static class Panning
{
    /// <param name="azimuthRadians">0 = front, positive = clockwise (right), range -pi..pi.</param>
    public static (float Left, float Right) Compute(float azimuthRadians)
    {
        float cos = MathF.Cos(azimuthRadians);
        float left = -0.37525f * cos + 0.625f;
        float right = 0.37525f * cos + 0.625f;
        return (left, right);
    }
}
