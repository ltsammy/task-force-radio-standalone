using NAudio.CoreAudioApi;
using NAudio.Wave;
using NAudio.Wave.SampleProviders;

namespace Tfrs.VoiceClient.Audio;

/// <summary>
/// Captures the microphone via WASAPI, converts to 48kHz mono float regardless of the device's
/// native format, and raises one event per complete 20ms Opus frame (960 samples).
/// </summary>
internal sealed class MicCaptureService : IDisposable
{
    private WasapiCapture? _capture;
    private BufferedWaveProvider? _rawBuffer;
    private ISampleProvider? _monoResampled;
    private float[] _frame = new float[OpusFormat.FrameSamples];
    private int _framePos;
    private readonly float[] _scratch = new float[OpusFormat.FrameSamples];

    public bool IsRunning { get; private set; }

    /// <summary>Raised on the capture callback thread — subscribers must not block.</summary>
    public event Action<float[]>? FrameCaptured;

    public void Start(MMDevice device)
    {
        Stop();

        _capture = new WasapiCapture(device) { ShareMode = AudioClientShareMode.Shared };
        _rawBuffer = new BufferedWaveProvider(_capture.WaveFormat)
        {
            DiscardOnBufferOverflow = true,
            BufferDuration = TimeSpan.FromSeconds(1),
        };

        ISampleProvider source = _rawBuffer.ToSampleProvider();
        if (source.WaveFormat.Channels > 1)
            source = new StereoToMonoSampleProvider(source) { LeftVolume = 1f, RightVolume = 1f };
        if (source.WaveFormat.SampleRate != OpusFormat.SampleRate)
            source = new WdlResamplingSampleProvider(source, OpusFormat.SampleRate);
        _monoResampled = source;

        _capture.DataAvailable += OnDataAvailable;
        _capture.StartRecording();
        IsRunning = true;
    }

    public void Stop()
    {
        if (_capture is null) return;
        IsRunning = false;
        _capture.DataAvailable -= OnDataAvailable;
        try { _capture.StopRecording(); } catch { /* already stopped */ }
        _capture.Dispose();
        _capture = null;
        _rawBuffer = null;
        _monoResampled = null;
        _framePos = 0;
    }

    private void OnDataAvailable(object? sender, WaveInEventArgs e)
    {
        if (_rawBuffer is null || _monoResampled is null) return;
        _rawBuffer.AddSamples(e.Buffer, 0, e.BytesRecorded);

        // Drain whatever the resampler can produce right now, in Opus-frame-sized chunks.
        while (true)
        {
            int wanted = Math.Min(OpusFormat.FrameSamples - _framePos, _scratch.Length);
            int read = _monoResampled.Read(_scratch, 0, wanted);
            if (read <= 0) break;

            Array.Copy(_scratch, 0, _frame, _framePos, read);
            _framePos += read;

            if (_framePos >= OpusFormat.FrameSamples)
            {
                FrameCaptured?.Invoke(_frame);
                _frame = new float[OpusFormat.FrameSamples];
                _framePos = 0;
            }

            if (read < wanted) break; // resampler/buffer temporarily drained
        }
    }

    public void Dispose() => Stop();
}
