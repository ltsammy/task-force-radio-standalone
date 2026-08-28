using Tfrs.VoiceServer;

var options = ServerOptions.FromEnvironmentAndArgs(args);

using var cts = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    cts.Cancel();
};
AppDomain.CurrentDomain.ProcessExit += (_, _) => cts.Cancel();

await using var server = new RelayServer(options);
await server.RunAsync(cts.Token);
