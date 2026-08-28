using NAudio.Wave;

namespace Tfrs.VoiceClient.Audio;

/// <summary>Wraps a sample provider to report its RMS level as it's read — used to drive the
/// output ("incoming voice") level meter. Fires on whatever thread pulls audio (the WASAPI render
/// thread), same as <see cref="TransmitController.LevelMeasured"/> on the capture side.</summary>
internal sealed class MeteringSampleProvider(ISampleProvider source) : ISampleProvider
{
    public WaveFormat WaveFormat => source.WaveFormat;

    public event Action<float>? LevelMeasured;

    public int Read(float[] buffer, int offset, int count)
    {
        int read = source.Read(buffer, offset, count);
        if (read > 0)
        {
            double sum = 0;
            for (int i = 0; i < read; i++)
            {
                float s = buffer[offset + i];
                sum += (double)s * s;
            }
            LevelMeasured?.Invoke((float)Math.Sqrt(sum / read));
        }
        return read;
    }
}
