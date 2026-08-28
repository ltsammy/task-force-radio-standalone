namespace Tfrs.VoiceServer;

internal sealed record ServerOptions
{
    public int Port { get; init; } = 9987;
    public string Password { get; init; } = string.Empty;
    public int MaxClients { get; init; } = 300;
    public int TimeoutSeconds { get; init; } = 20;

    public static ServerOptions FromEnvironmentAndArgs(string[] args)
    {
        var options = new ServerOptions
        {
            Port = ParseInt(Environment.GetEnvironmentVariable("TFRS_PORT"), 9987),
            Password = Environment.GetEnvironmentVariable("TFRS_PASSWORD") ?? string.Empty,
            MaxClients = ParseInt(Environment.GetEnvironmentVariable("TFRS_MAX_CLIENTS"), 300),
            TimeoutSeconds = ParseInt(Environment.GetEnvironmentVariable("TFRS_TIMEOUT_SECONDS"), 20),
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
            }
        }

        return options;
    }

    private static int ParseInt(string? value, int fallback) =>
        int.TryParse(value, out var parsed) ? parsed : fallback;
}
