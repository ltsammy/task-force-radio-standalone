// Wire protocol to Tfrs.VoiceServer (docs/protocol-network.md). Field-for-field port of
// voice-client/src/Tfrs.VoiceClient/Networking/Protocol.cs -- keep byte-exact, the server is
// unmodified and expects precisely this layout. UDP, little-endian, no framing beyond the
// PacketType byte at offset 0; strings are "string8": 1 length byte + raw UTF-8 bytes, not null
// terminated.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace tfrs {
namespace voice {

enum class PacketType : uint8_t {
    ConnectRequest = 1,     // C->S
    ConnectAccept = 2,      // S->C
    ConnectReject = 3,      // S->C
    Disconnect = 4,         // C->S (S->C direction unused today)
    Ping = 5,               // C->S
    Pong = 6,                // S->C
    VoiceUp = 7,             // C->S
    VoiceDown = 8,           // S->C
    ClientJoined = 9,        // S->C
    ClientLeft = 10,         // S->C
    RosterRequest = 11,      // C->S
    RadioTxUpdate = 12,      // C->S
    RadioTxBroadcast = 13,   // S->C
};

enum class ConnectRejectReason : uint8_t {
    BadPassword = 1,
    ServerFull = 2,
    VersionMismatch = 3,
    BadRequest = 4,
};

// [Flags] on the C# side; only one bit defined today.
enum VoiceFlags : uint8_t {
    kVoiceFlagsNone = 0,
    kVoiceFlagsLastFrame = 1 << 0,
};

constexpr uint8_t kVersionMajor = 1;
constexpr size_t kMaxNameLength = 32;
constexpr size_t kMaxUidLength = 40;
constexpr size_t kMaxOpusFrameLength = 500;
constexpr size_t kMaxFreqLength = 16;
constexpr size_t kMaxSubtypeLength = 16;
constexpr size_t kMaxVersionLength = 16;

// Truncates to at most `maxUtf8Bytes` UTF-8 bytes, only ever cutting at a UTF-8 sequence boundary
// (never splits a multi-byte codepoint) -- matches VoiceNetworkClient.cs's Truncate(). Callers must
// pre-truncate before writeString8(); the writer itself does not (matches the C# split between
// Truncate() and PacketWriter.WriteString8()).
std::string truncateUtf8(const std::string& value, size_t maxUtf8Bytes);

// Fixed-size, caller-owned output buffer. Size it to the protocol's documented per-packet maximum
// (see docs/protocol-network.md) before writing -- writeString8/writeBytes throw std::length_error
// if the buffer is too small, on the theory that's always a caller sizing bug, never live input
// (input length is what truncateUtf8 already bounded).
class PacketWriter {
public:
    PacketWriter(uint8_t* buffer, size_t capacity) : m_buffer(buffer), m_capacity(capacity) {}

    void writeByte(uint8_t value);
    void writeUInt16(uint16_t value);
    void writeUInt32(uint32_t value);
    void writeString8(const std::string& value);
    void writeBytes(const uint8_t* data, size_t len);

    const uint8_t* data() const { return m_buffer; }
    size_t size() const { return m_pos; }

private:
    void ensure(size_t more);

    uint8_t* m_buffer;
    size_t m_capacity;
    size_t m_pos = 0;
};

// Read-only view over one received datagram. Throws std::runtime_error on a truncated/malformed
// field (matches ReadString8's InvalidDataException) -- callers (VoiceNetworkClient's receive
// loop) must catch per-datagram and drop just that packet, exactly like the C# reference ("any
// parse exception inside HandlePacket is caught and ignored").
class PacketReader {
public:
    PacketReader(const uint8_t* buffer, size_t size) : m_buffer(buffer), m_size(size) {}

    size_t remaining() const { return m_size - m_pos; }
    uint8_t readByte();
    uint16_t readUInt16();
    uint32_t readUInt32();
    std::string readString8(size_t maxLength);
    // Only valid as the last field of a message (matches ReadRemaining()) -- e.g. the opus payload
    // on VoiceUp/VoiceDown, which carries no length prefix of its own.
    const uint8_t* readRemaining(size_t& outLen);

private:
    void need(size_t n) const;

    const uint8_t* m_buffer;
    size_t m_size;
    size_t m_pos = 0;
};

}  // namespace voice
}  // namespace tfrs
