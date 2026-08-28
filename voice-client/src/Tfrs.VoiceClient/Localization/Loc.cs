namespace Tfrs.VoiceClient.Localization;

internal enum AppLanguage { En, De }

/// <summary>
/// Minimal hand-rolled localization: two flat dictionaries, no resource files/.resx, no
/// dynamic language switching at runtime (the language is fixed for the process's lifetime via
/// --lang, decided once in App.xaml.cs before any window is created). Good enough for a small,
/// fixed set of UI strings without pulling in a full i18n framework.
/// </summary>
internal static class Loc
{
    public static AppLanguage Language { get; set; } = AppLanguage.En;

    public static string Get(string key)
    {
        var table = Language == AppLanguage.De ? De : En;
        return table.TryGetValue(key, out var value) ? value : key;
    }

    public static string Format(string key, params object[] args) => string.Format(Get(key), args);

    private static readonly Dictionary<string, string> En = new()
    {
        ["AppTitle"] = "Task Force Radio Standalone",
        ["VoiceClientSubtitle"] = "Voice Client",

        ["ServerHeader"] = "SERVER",
        ["IpHostLabel"] = "IP / Host",
        ["PortLabel"] = "Port",
        ["PasswordLabel"] = "Password",
        ["ConnectButton"] = "Connect",
        ["DisconnectButton"] = "Disconnect",
        ["StatusNotConnected"] = "Not connected",
        ["StatusConnected"] = "Connected",
        ["StatusConnectedSending"] = "Connected — sending…",
        ["MicMutedBadge"] = "  🎤 MUTED",
        ["SpeakerMutedBadge"] = "  🔇 MUTED",
        ["ConnectedCount"] = "Connected clients: {0}",

        ["LevelsHeader"] = "LEVELS",
        ["MicrophoneLabel"] = "Microphone",
        ["IncomingLabel"] = "Incoming",
        ["MicOn"] = "Mic: On",
        ["MicMuted"] = "Mic: MUTED",
        ["SpeakerOn"] = "Speaker: On",
        ["SpeakerMuted"] = "Speaker: MUTED",
        ["SettingsButton"] = "⚙ Settings",
        ["ExtensionNotConnected"] = "Arma addon: not connected",
        ["ExtensionConnected"] = "Arma addon: connected",

        ["SettingsTitle"] = "Settings — Task Force Radio Standalone",
        ["AudioDevicesHeader"] = "AUDIO DEVICES",
        ["MicrophoneVolumeLabel"] = "Microphone volume",
        ["SpeakerHeadsetLabel"] = "Speaker/headset",
        ["PlaybackVolumeLabel"] = "Playback volume",
        ["SystemDefault"] = "System default",

        ["TransmitModeHeader"] = "TRANSMIT MODE",
        ["PushToTalk"] = "Push-to-talk",
        ["VoiceActivation"] = "Voice activation",
        ["AlwaysOn"] = "Always on",
        ["VadSensitivityLabel"] = "VAD sensitivity",

        ["KeysHeader"] = "KEYS",
        ["MicMuteLabel"] = "Mute microphone",
        ["SpeakerMuteLabel"] = "Mute speaker",
        ["NotBound"] = "(not bound)",
        ["PressAKey"] = "Press a key…",

        ["LogHeader"] = "LOG",
        ["CloseButton"] = "Close",

        ["InvalidPort"] = "Invalid port.",
        ["ConnectedTo"] = "Connected to {0}:{1}.",
        ["Disconnected"] = "Disconnected.",
        ["ConnectFailed"] = "Connection failed: {0}",
        ["SilenceHint"] = "No microphone signal for 3s. Check: (1) Windows Settings → Privacy & security → Microphone → the top toggle \"Microphone access\" AND \"Let desktop apps access your microphone\" — both must be on; (2) the correct device is selected in Settings here; (3) the mic isn't muted at the OS level (see below) or by a hardware mute switch.",
        ["SilenceHintShort"] = "⚠ No mic signal — see Settings → Log for things to check",
        ["MicMutedAtOsHint"] = "⚠ Your microphone is muted (or at 0 volume) in Windows — right-click the speaker icon → Sound settings → Input → your microphone, and unmute/turn it up.",

        ["DnsFailed"] = "DNS resolution failed: {0}",
        ["HostNotResolved"] = "Could not resolve host.",
        ["SendFailed"] = "Send failed: {0}",
        ["ServerTimeout"] = "Server is not responding (timeout).",
        ["BadPassword"] = "Wrong password.",
        ["ServerFull"] = "Server is full.",
        ["VersionMismatch"] = "Protocol version doesn't match the server.",
        ["ConnectionRejected"] = "Connection rejected by the server.",
    };

    private static readonly Dictionary<string, string> De = new()
    {
        ["AppTitle"] = "Task Force Radio Standalone",
        ["VoiceClientSubtitle"] = "Voice Client",

        ["ServerHeader"] = "SERVER",
        ["IpHostLabel"] = "IP / Host",
        ["PortLabel"] = "Port",
        ["PasswordLabel"] = "Passwort",
        ["ConnectButton"] = "Verbinden",
        ["DisconnectButton"] = "Trennen",
        ["StatusNotConnected"] = "Nicht verbunden",
        ["StatusConnected"] = "Verbunden",
        ["StatusConnectedSending"] = "Verbunden — sendet…",
        ["MicMutedBadge"] = "  🎤 STUMM",
        ["SpeakerMutedBadge"] = "  🔇 STUMM",
        ["ConnectedCount"] = "Verbundene Clients: {0}",

        ["LevelsHeader"] = "PEGEL",
        ["MicrophoneLabel"] = "Mikrofon",
        ["IncomingLabel"] = "Eingehend",
        ["MicOn"] = "Mikrofon: An",
        ["MicMuted"] = "Mikrofon: STUMM",
        ["SpeakerOn"] = "Lautsprecher: An",
        ["SpeakerMuted"] = "Lautsprecher: STUMM",
        ["SettingsButton"] = "⚙ Einstellungen",
        ["ExtensionNotConnected"] = "Arma-Addon: nicht verbunden",
        ["ExtensionConnected"] = "Arma-Addon: verbunden",

        ["SettingsTitle"] = "Einstellungen — Task Force Radio Standalone",
        ["AudioDevicesHeader"] = "AUDIOGERÄTE",
        ["MicrophoneVolumeLabel"] = "Mikrofon-Lautstärke",
        ["SpeakerHeadsetLabel"] = "Lautsprecher/Headset",
        ["PlaybackVolumeLabel"] = "Wiedergabe-Lautstärke",
        ["SystemDefault"] = "Systemstandard",

        ["TransmitModeHeader"] = "SENDEMODUS",
        ["PushToTalk"] = "Push-to-Talk",
        ["VoiceActivation"] = "Sprachaktivierung",
        ["AlwaysOn"] = "Dauersenden",
        ["VadSensitivityLabel"] = "VAD-Empfindlichkeit",

        ["KeysHeader"] = "TASTEN",
        ["MicMuteLabel"] = "Mikrofon stumm",
        ["SpeakerMuteLabel"] = "Lautsprecher stumm",
        ["NotBound"] = "(nicht gebunden)",
        ["PressAKey"] = "Taste drücken…",

        ["LogHeader"] = "PROTOKOLL",
        ["CloseButton"] = "Schließen",

        ["InvalidPort"] = "Ungültiger Port.",
        ["ConnectedTo"] = "Verbunden mit {0}:{1}.",
        ["Disconnected"] = "Getrennt.",
        ["ConnectFailed"] = "Verbindung fehlgeschlagen: {0}",
        ["SilenceHint"] = "Kein Mikrofonsignal seit 3s. Prüfe: (1) Windows-Einstellungen → Datenschutz und Sicherheit → Mikrofon → den oberen Schalter \"Mikrofonzugriff\" UND \"Desktop-Apps auf das Mikrofon zugreifen lassen\" — beide müssen an sein; (2) ob in den Einstellungen hier das richtige Gerät ausgewählt ist; (3) ob das Mikrofon auf Windows-Ebene stummgeschaltet ist (siehe unten) oder ein Hardware-Mute-Schalter aktiv ist.",
        ["SilenceHintShort"] = "⚠ Kein Mikrofonsignal — siehe Einstellungen → Protokoll für Details",
        ["MicMutedAtOsHint"] = "⚠ Dein Mikrofon ist unter Windows stummgeschaltet (oder auf 0 Lautstärke) — Rechtsklick auf das Lautsprecher-Symbol → Soundeinstellungen → Eingabe → dein Mikrofon, dann entstummen/lauter stellen.",

        ["DnsFailed"] = "DNS-Auflösung fehlgeschlagen: {0}",
        ["HostNotResolved"] = "Host konnte nicht aufgelöst werden.",
        ["SendFailed"] = "Senden fehlgeschlagen: {0}",
        ["ServerTimeout"] = "Server antwortet nicht (Timeout).",
        ["BadPassword"] = "Falsches Passwort.",
        ["ServerFull"] = "Server ist voll.",
        ["VersionMismatch"] = "Protokollversion passt nicht zum Server.",
        ["ConnectionRejected"] = "Verbindung vom Server abgelehnt.",
    };
}
