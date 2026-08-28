namespace Tfrs.VoiceClient.Audio.Dsp;

/// <summary>
/// Simplified stereo panning (docs/dsp-audio-pipeline.md section 2) — a deliberate stand-in for the
/// original's Clunk/KEMAR HRTF convolution, using the same gain law TFAR itself used for speaker
/// radios. Good enough directional cue for a stereo setup without a binaural convolution engine.
/// </summary>
internal static class Panning
{
    /// <param name="azimuthRadians">0 = front, positive = clockwise (right), range -pi..pi.
    /// Uses sin(), not cos(): the original formula's angle is measured from the right axis, not
    /// from "front" — cos() against this azimuth convention would pan dead-ahead audio hard to one
    /// side instead of centering it. Caught while cross-checking against the extension's az
    /// convention, see addon/extensions/task_force_radio_pipe/README.md.</param>
    public static (float Left, float Right) Compute(float azimuthRadians)
    {
        float sin = MathF.Sin(azimuthRadians);
        float left = -0.37525f * sin + 0.625f;
        float right = 0.37525f * sin + 0.625f;
        return (left, right);
    }
}
