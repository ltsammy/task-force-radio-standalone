// Thin libopus wrapper. Params match voice-client's OpusCodec.cs (OpusFormat) exactly: 48kHz
// mono VOIP, 24kbps VBR, complexity 6, 20ms/960-sample frames -- keep these in sync with the
// server/other clients' expectations, they're not independently negotiable per-connection.
#pragma once

#include <cstddef>
#include <cstdint>

struct OpusEncoder;
struct OpusDecoder;

namespace tfrs {
namespace voice {

struct OpusFormat {
    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels = 1;
    static constexpr int kFrameMillis = 20;
    static constexpr int kFrameSamples = 960;  // 48000 * 20 / 1000
    static constexpr int kBitrate = 24000;
};

class OpusVoiceEncoder {
public:
    OpusVoiceEncoder();
    ~OpusVoiceEncoder();
    OpusVoiceEncoder(const OpusVoiceEncoder&) = delete;
    OpusVoiceEncoder& operator=(const OpusVoiceEncoder&) = delete;

    // Encodes exactly one 20ms/960-sample frame. Returns the encoded byte count, or 0 on failure
    // (encoder never constructed, or opus_encode itself failed). `outCapacity` should be at least
    // Protocol::kMaxOpusFrameLength bytes.
    int encode(const int16_t* pcm, uint8_t* out, int outCapacity);

private:
    OpusEncoder* m_encoder = nullptr;
};

class OpusVoiceDecoder {
public:
    OpusVoiceDecoder();
    ~OpusVoiceDecoder();
    OpusVoiceDecoder(const OpusVoiceDecoder&) = delete;
    OpusVoiceDecoder& operator=(const OpusVoiceDecoder&) = delete;

    // Decodes into exactly kFrameSamples of PCM. Never leaves `outPcm` uninitialized -- writes
    // silence if the decoder wasn't constructed or opus_decode itself fails, so callers can always
    // treat the output buffer as valid.
    void decode(const uint8_t* payload, int payloadLen, int16_t* outPcm);
    // Packet-loss concealment: synthesizes a plausible continuation frame from decoder state
    // (opus_decode with a null payload) -- used when a frame is expected but never arrived.
    void decodePacketLoss(int16_t* outPcm);

private:
    OpusDecoder* m_decoder = nullptr;
};

}  // namespace voice
}  // namespace tfrs
