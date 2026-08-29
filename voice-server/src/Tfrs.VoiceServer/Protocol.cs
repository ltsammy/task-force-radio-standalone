using System.Buffers.Binary;
using System.Text;

namespace Tfrs.VoiceServer;

/// <summary>
/// Wire protocol for the TFRS voice relay (client &lt;-&gt; server), UDP, little-endian.
/// Mirrored by Tfrs.VoiceClient's Protocol.cs (kept in sync manually — see docs/protocol-network.md).
/// </summary>
internal static class Protocol
{
    public const byte VersionMajor = 1;

    public const int MaxUdpPayload = 1200; // safe below typical MTU to avoid fragmentation
    public const int MaxNameLength = 32;
    public const int MaxUidLength = 40;
    public const int MaxOpusFrameLength = 500;
    public const int MaxFreqLength = 16;
    public const int MaxSubtypeLength = 16;
}

internal enum PacketType : byte
{
    ConnectRequest = 1,
    ConnectAccept = 2,
    ConnectReject = 3,
    Disconnect = 4,
    Ping = 5,
    Pong = 6,
    VoiceUp = 7,
    VoiceDown = 8,
    ClientJoined = 9,
    ClientLeft = 10,
    RosterRequest = 11,
    RadioTxUpdate = 12,
    RadioTxBroadcast = 13,
}

internal enum ConnectRejectReason : byte
{
    BadPassword = 1,
    ServerFull = 2,
    VersionMismatch = 3,
    BadRequest = 4,
}

internal enum DisconnectReason : byte
{
    ClientRequested = 0,
    Timeout = 1,
    Kicked = 2,
    ServerShutdown = 3,
}

[Flags]
internal enum VoiceFlags : byte
{
    None = 0,
    LastFrame = 1 << 0,
}

/// <summary>Minimal span-based writer for the fixed little-endian binary protocol.</summary>
internal ref struct PacketWriter
{
    private readonly Span<byte> _buffer;
    private int _pos;

    public PacketWriter(Span<byte> buffer)
    {
        _buffer = buffer;
        _pos = 0;
    }

    public readonly int Length => _pos;
    public readonly ReadOnlySpan<byte> Written => _buffer[.._pos];

    public void WriteByte(byte value) => _buffer[_pos++] = value;

    public void WriteUInt16(ushort value)
    {
        BinaryPrimitives.WriteUInt16LittleEndian(_buffer[_pos..], value);
        _pos += 2;
    }

    public void WriteUInt32(uint value)
    {
        BinaryPrimitives.WriteUInt32LittleEndian(_buffer[_pos..], value);
        _pos += 4;
    }

    /// <summary>Writes a UTF-8 string prefixed with a single length byte (max 255 bytes).</summary>
    public void WriteString8(string value)
    {
        int byteCount = Encoding.UTF8.GetBytes(value, _buffer[(_pos + 1)..]);
        _buffer[_pos] = (byte)byteCount;
        _pos += 1 + byteCount;
    }

    public void WriteBytes(ReadOnlySpan<byte> bytes)
    {
        bytes.CopyTo(_buffer[_pos..]);
        _pos += bytes.Length;
    }
}

/// <summary>Minimal span-based reader counterpart to <see cref="PacketWriter"/>.</summary>
internal ref struct PacketReader
{
    private readonly ReadOnlySpan<byte> _buffer;
    private int _pos;

    public PacketReader(ReadOnlySpan<byte> buffer)
    {
        _buffer = buffer;
        _pos = 0;
    }

    public readonly bool HasMore => _pos < _buffer.Length;
    public readonly int Remaining => _buffer.Length - _pos;

    public byte ReadByte() => _buffer[_pos++];

    public ushort ReadUInt16()
    {
        ushort value = BinaryPrimitives.ReadUInt16LittleEndian(_buffer[_pos..]);
        _pos += 2;
        return value;
    }

    public uint ReadUInt32()
    {
        uint value = BinaryPrimitives.ReadUInt32LittleEndian(_buffer[_pos..]);
        _pos += 4;
        return value;
    }

    public byte[] ReadBytesFixed(int count)
    {
        if (_pos + count > _buffer.Length)
            throw new InvalidDataException("Not enough data for fixed-length field.");
        var value = _buffer.Slice(_pos, count).ToArray();
        _pos += count;
        return value;
    }

    public string ReadString8(int maxLength)
    {
        int byteCount = _buffer[_pos];
        _pos += 1;
        if (byteCount > maxLength || _pos + byteCount > _buffer.Length)
            throw new InvalidDataException("String field exceeds allowed/available length.");
        string value = Encoding.UTF8.GetString(_buffer.Slice(_pos, byteCount));
        _pos += byteCount;
        return value;
    }

    public ReadOnlySpan<byte> ReadRemaining()
    {
        var slice = _buffer[_pos..];
        _pos = _buffer.Length;
        return slice;
    }
}
