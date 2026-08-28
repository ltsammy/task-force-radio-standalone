using System.Collections.Concurrent;
using NAudio.CoreAudioApi;
using NAudio.Wave;
using NAudio.Wave.SampleProviders;

namespace Tfrs.VoiceClient.Audio;

/// <summary>Zeroes the wrapped provider's output while <see cref="IsMuted"/> is set, without
/// stopping/reopening the output device (avoids audible clicks and re-open latency on toggle).</summary>
internal sealed class MuteGateSampleProvider(ISampleProvider source) : ISampleProvider
{
    public bool IsMuted { get; set; }
    public WaveFormat WaveFormat => source.WaveFormat;

    public int Read(float[] buffer, int offset, int count)
    {
        int read = source.Read(buffer, offset, count);
        if (IsMuted) Array.Clear(buffer, offset, read);
        return read;
    }
}

/// <summary>Owns the output device and the mix of all currently-known remote voice sources.</summary>
internal sealed class PlaybackEngine : IDisposable
{
    private readonly MixingSampleProvider _mixer;
    private readonly VolumeSampleProvider _masterVolume;
    private readonly MuteGateSampleProvider _muteGate;
    private readonly ConcurrentDictionary<uint, RemoteVoiceSource> _sources = new();
    private WasapiOut? _output;

    public bool SpeakerMuted
    {
        get => _muteGate.IsMuted;
        set => _muteGate.IsMuted = value;
    }

    public float MasterVolume
    {
        get => _masterVolume.Volume;
        set => _masterVolume.Volume = Math.Clamp(value, 0f, 2f);
    }

    public PlaybackEngine()
    {
        _mixer = new MixingSampleProvider(WaveFormat.CreateIeeeFloatWaveFormat(OpusFormat.SampleRate, 2))
        {
            ReadFully = true,
        };
        _masterVolume = new VolumeSampleProvider(_mixer) { Volume = 1f };
        _muteGate = new MuteGateSampleProvider(_masterVolume);
    }

    public void Start(MMDevice device)
    {
        Stop();
        _output = new WasapiOut(device, AudioClientShareMode.Shared, true, 40);
        _output.Init(_muteGate.ToWaveProvider());
        _output.Play();
    }

    public void Stop()
    {
        if (_output is null) return;
        try { _output.Stop(); } catch { /* already stopped */ }
        _output.Dispose();
        _output = null;
    }

    public RemoteVoiceSource GetOrAddSource(uint sessionId, string uid) =>
        _sources.GetOrAdd(sessionId, id =>
        {
            var source = new RemoteVoiceSource(id, uid);
            _mixer.AddMixerInput(source);
            return source;
        });

    public bool TryGetSource(uint sessionId, out RemoteVoiceSource source) =>
        _sources.TryGetValue(sessionId, out source!);

    public void RemoveSource(uint sessionId)
    {
        if (_sources.TryRemove(sessionId, out var source))
            _mixer.RemoveMixerInput(source);
    }

    public void Dispose()
    {
        Stop();
    }
}
