using NAudio.CoreAudioApi;

namespace Tfrs.VoiceClient.Audio;

internal sealed record AudioDeviceInfo(string Id, string Name);

internal static class AudioDevices
{
    public static IReadOnlyList<AudioDeviceInfo> ListInputs() =>
        List(DataFlow.Capture);

    public static IReadOnlyList<AudioDeviceInfo> ListOutputs() =>
        List(DataFlow.Render);

    private static IReadOnlyList<AudioDeviceInfo> List(DataFlow flow)
    {
        using var enumerator = new MMDeviceEnumerator();
        var result = new List<AudioDeviceInfo>();
        foreach (var device in enumerator.EnumerateAudioEndPoints(flow, DeviceState.Active))
        {
            result.Add(new AudioDeviceInfo(device.ID, device.FriendlyName));
            device.Dispose();
        }
        return result;
    }

    public static MMDevice? Resolve(string? deviceId, DataFlow flow)
    {
        using var enumerator = new MMDeviceEnumerator();
        if (!string.IsNullOrEmpty(deviceId))
        {
            try { return enumerator.GetDevice(deviceId); }
            catch { /* device unplugged since last run — fall back to default */ }
        }

        // Role.Multimedia matches the "Default Device" shown in Windows Sound Settings — what a
        // user means by "my default mic/speaker". Role.Communications can silently point at a
        // *different* device (Windows lets the two be set independently via the legacy Recording
        // Devices control panel), which looks like "it's just not picking up my mic" with no error.
        try { return enumerator.GetDefaultAudioEndpoint(flow, Role.Multimedia); }
        catch { return null; }
    }

    /// <summary>WASAPI capture on a muted endpoint doesn't throw or fail — it just delivers
    /// correctly-sized buffers full of zero bytes forever, which is indistinguishable from "no
    /// signal" from inside the capture callback. Checking this directly (mute switch/icon under
    /// Sound Settings → Input, or a hardware mute button on a headset) is far more reliable than
    /// inferring it from a silence heuristic, and is a completely different setting from the
    /// microphone *privacy* permission ("let desktop apps access the microphone").</summary>
    public static (bool muted, float volumeScalar) GetEndpointVolumeState(MMDevice device)
    {
        try
        {
            var vol = device.AudioEndpointVolume;
            return (vol.Mute, vol.MasterVolumeLevelScalar);
        }
        catch
        {
            return (false, 1f);
        }
    }
}
