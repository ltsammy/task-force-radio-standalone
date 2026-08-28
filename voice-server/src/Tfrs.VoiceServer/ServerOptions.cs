namespace Tfrs.VoiceServer;

internal sealed record ServerOptions
{
    public int Port { get; init; } = 9987;
    public string Password { get; init; } = string.Empty;
    public int MaxClients { get; init; } = 300;
    public int TimeoutSeconds { get; init; } = 20;

    /// <summary>Server-dictated only — sent to every client in ConnectAccept, never something a
    /// client can request or enable for itself (see VoiceSessionCoordinator/RemoteVoiceSource):
    /// forces every remote voice to full volume/no panning, bypassing the Arma extension's
    /// distance/gain computation entirely. Lets you verify mic-to-speaker transport in isolation
    /// (e.g. two bare clients, no Arma running at all) without trusting position logic that might
    /// itself be broken. If a client could flip this on locally it would let anyone hear other
    /// players regardless of real in-game distance -- an obvious cheat, so it must stay
    /// server-only.</summary>
    public bool DebugForceAudible { get; init; }

    public static ServerOptions FromEnvironmentAndArgs(string[] args)
    {
        var options = new ServerOptions
        {
            Port = ParseInt(Environment.GetEnvironmentVariable("TFRS_PORT"), 9987),
            Password = Environment.GetEnvironmentVariable("TFRS_PASSWORD") ?? string.Empty,
            MaxClients = ParseInt(Environment.GetEnvironmentVariable("TFRS_MAX_CLIENTS"), 300),
            TimeoutSeconds = ParseInt(Environment.GetEnvironmentVariable("TFRS_TIMEOUT_SECONDS"), 20),
            DebugForceAudible = ParseBool(Environment.GetEnvironmentVariable("TFRS_DEBUG_FORCE_AUDIBLE")),
        };

        // CLI args override env vars, mainly for convenient local testing outside Docker.
        for (int i = 0; i < args.Length - 1; i++)
        {
            switch (args[i])
            {
                case "--port":
                    options = options with { Port = ParseInt(args[i + 1], options.Port) };
                    break;
                case "--password":
                    options = options with { Password = args[i + 1] };
                    break;
                case "--max-clients":
                    options = options with { MaxClients = ParseInt(args[i + 1], options.MaxClients) };
                    break;
                case "--timeout-seconds":
                    options = options with { TimeoutSeconds = ParseInt(args[i + 1], options.TimeoutSeconds) };
                    break;
                case "--debug-force-audible":
                    options = options with { DebugForceAudible = ParseBool(args[i + 1]) };
                    break;
            }
        }

        return options;
    }

    private static int ParseInt(string? value, int fallback) =>
        int.TryParse(value, out var parsed) ? parsed : fallback;

    private static bool ParseBool(string? value) =>
        value is "1" or "true" or "TRUE" or "True";
}
