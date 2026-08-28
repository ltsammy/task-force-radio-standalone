namespace Tfrs.VoiceClient;

/// <summary>Startparameter, damit ein externer Launcher (z. B. der C#-Strikelauncher) den Client
/// direkt mit Server-IP/Port/Passwort starten kann, ohne dass der Nutzer sie eintippen muss.</summary>
internal sealed record CommandLineArgs(string? Host, int? Port, string? Password, string? Uid, string? Language, bool AutoConnect)
{
    public static CommandLineArgs Parse(string[] args)
    {
        string? host = null, password = null, uid = null, language = null;
        int? port = null;
        bool autoConnect = false;

        for (int i = 0; i < args.Length; i++)
        {
            string arg = args[i].TrimStart('-').ToLowerInvariant();
            switch (arg)
            {
                case "ip":
                case "host":
                    if (i + 1 < args.Length) host = args[++i];
                    break;
                case "port":
                    if (i + 1 < args.Length && int.TryParse(args[i + 1], out int p)) { port = p; i++; }
                    break;
                case "password":
                case "pw":
                    if (i + 1 < args.Length) password = args[++i];
                    break;
                // Should match the Arma player UID (e.g. Steam64 ID) so the extension's bridge
                // "units" messages — which reference this same UID — resolve to this connection.
                // A launcher that already knows the logged-in Steam account should pass this.
                case "uid":
                    if (i + 1 < args.Length) uid = args[++i];
                    break;
                // UI language: "en" (default) or "de".
                case "lang":
                case "language":
                    if (i + 1 < args.Length) language = args[++i];
                    break;
                case "autoconnect":
                    autoConnect = true;
                    break;
            }
        }

        if (host is not null) autoConnect = true;
        return new CommandLineArgs(host, port, password, uid, language, autoConnect);
    }
}
