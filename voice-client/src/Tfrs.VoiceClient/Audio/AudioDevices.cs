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

        try { return enumerator.GetDefaultAudioEndpoint(flow, Role.Communications); }
        catch { return null; }
    }
}
