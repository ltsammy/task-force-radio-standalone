using System.IO;
using System.Text.Json;
using Tfrs.VoiceClient.Audio;

namespace Tfrs.VoiceClient.Settings;

internal sealed class AppSettings
{
    public string ServerHost { get; set; } = "";
    public int ServerPort { get; set; } = 9987;
    /// <summary>Should match the Arma player UID (Steam64 ID) — see CommandLineArgs.Uid. Empty
    /// until a launcher/CLI provides it or the user sets it manually.</summary>
    public string LocalUid { get; set; } = "";

    // Persisted for connect-once convenience (this targets private, password-gated game servers,
    // not a security boundary worth forcing re-entry every launch — see docs/protocol-network.md).
    public string ServerPassword { get; set; } = "";

    public string? InputDeviceId { get; set; }
    public string? OutputDeviceId { get; set; }
    public float MicVolume { get; set; } = 1.0f;
    public float SpeakerVolume { get; set; } = 1.0f;
    public TransmitMode TransmitMode { get; set; } = TransmitMode.VoiceActivation;
    public float VadThreshold { get; set; } = 0.01f;

    /// <summary>Win32 virtual-key codes; 0 = unbound.</summary>
    public int PttKey { get; set; }
    public int MicMuteKey { get; set; }
    public int SpeakerMuteKey { get; set; }

    private static string SettingsPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Tfrs", "VoiceClient", "settings.json");

    // Save() is called from both the UI thread (every slider drag tick) and the addon bridge's
    // background pipe thread (VoiceSessionCoordinator.OnLocalUidReceived) -- two concurrent,
    // unsynchronized File.WriteAllText calls to the same path can interleave into a corrupted
    // file, which Load()'s catch-all then silently treats as "reset every setting to default".
    // This lock is what actually fixes that; it was the real cause behind an apparent
    // "VAD sensitivity keeps resetting itself" report.
    private static readonly object SaveLock = new();

    public static AppSettings Load()
    {
        try
        {
            string json = File.ReadAllText(SettingsPath);
            return JsonSerializer.Deserialize<AppSettings>(json) ?? new AppSettings();
        }
        catch
        {
            return new AppSettings();
        }
    }

    public void Save()
    {
        try
        {
            string path = SettingsPath;
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            string json = JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true });
            lock (SaveLock) File.WriteAllText(path, json);
        }
        catch
        {
            // best-effort persistence — a failed save shouldn't crash the app
        }
    }
}
