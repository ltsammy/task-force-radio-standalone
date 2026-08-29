#include "Protocol.h"

#include <cstring>
#include <stdexcept>

namespace tfrs {
namespace voice {

std::string truncateUtf8(const std::string& value, size_t maxUtf8Bytes) {
    std::string result = value;
    while (result.size() > maxUtf8Bytes) {
        // Drop the last codepoint, not just the last byte: a UTF-8 continuation byte has its two
        // high bits set to 10, so walk back to the start of the last sequence before erasing.
        size_t cut = result.size() - 1;
        while (cut > 0 && (static_cast<unsigned char>(result[cut]) & 0xC0) == 0x80) --cut;
        result.erase(cut);
    }
    return result;
}

void PacketWriter::ensure(size_t more) {
    if (m_pos + more > m_capacity) throw std::length_error("PacketWriter: buffer too small");
}

void PacketWriter::writeByte(uint8_t value) {
    ensure(1);
    m_buffer[m_pos++] = value;
}

void PacketWriter::writeUInt16(uint16_t value) {
    ensure(2);
    m_buffer[m_pos++] = static_cast<uint8_t>(value & 0xFF);
    m_buffer[m_pos++] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void PacketWriter::writeUInt32(uint32_t value) {
    ensure(4);
    m_buffer[m_pos++] = static_cast<uint8_t>(value & 0xFF);
    m_buffer[m_pos++] = static_cast<uint8_t>((value >> 8) & 0xFF);
    m_buffer[m_pos++] = static_cast<uint8_t>((value >> 16) & 0xFF);
    m_buffer[m_pos++] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void PacketWriter::writeString8(const std::string& value) {
    // The 1-byte length prefix caps this at 255 regardless of caller intent -- every real caller
    // pre-truncates to a protocol max (<=40) via truncateUtf8() first, so this only ever fires on
    // a genuine caller bug, not on live data.
    if (value.size() > 255) throw std::length_error("PacketWriter::writeString8: value too long");
    ensure(1 + value.size());
    m_buffer[m_pos++] = static_cast<uint8_t>(value.size());
    if (!value.empty()) {
        std::memcpy(m_buffer + m_pos, value.data(), value.size());
        m_pos += value.size();
    }
}

void PacketWriter::writeBytes(const uint8_t* data, size_t len) {
    ensure(len);
    if (len > 0) {
        std::memcpy(m_buffer + m_pos, data, len);
        m_pos += len;
    }
}

void PacketReader::need(size_t n) const {
    if (remaining() < n) throw std::runtime_error("PacketReader: truncated packet");
}

uint8_t PacketReader::readByte() {
    need(1);
    return m_buffer[m_pos++];
}

uint16_t PacketReader::readUInt16() {
    need(2);
    const uint16_t value = static_cast<uint16_t>(m_buffer[m_pos]) |
                            (static_cast<uint16_t>(m_buffer[m_pos + 1]) << 8);
    m_pos += 2;
    return value;
}

uint32_t PacketReader::readUInt32() {
    need(4);
    const uint32_t value = static_cast<uint32_t>(m_buffer[m_pos]) |
                            (static_cast<uint32_t>(m_buffer[m_pos + 1]) << 8) |
                            (static_cast<uint32_t>(m_buffer[m_pos + 2]) << 16) |
                            (static_cast<uint32_t>(m_buffer[m_pos + 3]) << 24);
    m_pos += 4;
    return value;
}

std::string PacketReader::readString8(size_t maxLength) {
    const uint8_t len = readByte();
    if (len > maxLength) throw std::runtime_error("PacketReader: string field exceeds max length");
    need(len);
    std::string value(reinterpret_cast<const char*>(m_buffer + m_pos), len);
    m_pos += len;
    return value;
}

const uint8_t* PacketReader::readRemaining(size_t& outLen) {
    outLen = remaining();
    const uint8_t* p = m_buffer + m_pos;
    m_pos = m_size;
    return p;
}

}  // namespace voice
}  // namespace tfrs
