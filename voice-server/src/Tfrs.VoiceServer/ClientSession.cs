using System.Net;

namespace Tfrs.VoiceServer;

internal sealed class ClientSession
{
    public required uint SessionId { get; init; }
    public required IPEndPoint EndPoint { get; set; }
    public required string Uid { get; init; }
    public required string Name { get; init; }
    public DateTime LastSeenUtc { get; set; } = DateTime.UtcNow;
    public ushort LastVoiceSequence { get; set; }
}
