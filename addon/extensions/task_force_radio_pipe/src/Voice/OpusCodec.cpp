#include "OpusCodec.h"

#include <cstring>

#include <opus.h>

namespace tfrs {
namespace voice {

OpusVoiceEncoder::OpusVoiceEncoder() {
    int err = OPUS_OK;
    m_encoder = opus_encoder_create(OpusFormat::kSampleRate, OpusFormat::kChannels,
                                     OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || m_encoder == nullptr) {
        m_encoder = nullptr;
        return;
    }
    opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(OpusFormat::kBitrate));
    opus_encoder_ctl(m_encoder, OPUS_SET_COMPLEXITY(6));
    opus_encoder_ctl(m_encoder, OPUS_SET_VBR(1));
}

OpusVoiceEncoder::~OpusVoiceEncoder() {
    if (m_encoder != nullptr) opus_encoder_destroy(m_encoder);
}

int OpusVoiceEncoder::encode(const int16_t* pcm, uint8_t* out, int outCapacity) {
    if (m_encoder == nullptr) return 0;
    const int result =
        opus_encode(m_encoder, pcm, OpusFormat::kFrameSamples, out, outCapacity);
    return result > 0 ? result : 0;
}

OpusVoiceDecoder::OpusVoiceDecoder() {
    int err = OPUS_OK;
    m_decoder = opus_decoder_create(OpusFormat::kSampleRate, OpusFormat::kChannels, &err);
    if (err != OPUS_OK || m_decoder == nullptr) m_decoder = nullptr;
}

OpusVoiceDecoder::~OpusVoiceDecoder() {
    if (m_decoder != nullptr) opus_decoder_destroy(m_decoder);
}

void OpusVoiceDecoder::decode(const uint8_t* payload, int payloadLen, int16_t* outPcm) {
    if (m_decoder != nullptr) {
        const int decoded =
            opus_decode(m_decoder, payload, payloadLen, outPcm, OpusFormat::kFrameSamples, 0);
        if (decoded == OpusFormat::kFrameSamples) return;
    }
    std::memset(outPcm, 0, sizeof(int16_t) * static_cast<size_t>(OpusFormat::kFrameSamples));
}

void OpusVoiceDecoder::decodePacketLoss(int16_t* outPcm) {
    if (m_decoder != nullptr) {
        const int decoded =
            opus_decode(m_decoder, nullptr, 0, outPcm, OpusFormat::kFrameSamples, 0);
        if (decoded == OpusFormat::kFrameSamples) return;
    }
    std::memset(outPcm, 0, sizeof(int16_t) * static_cast<size_t>(OpusFormat::kFrameSamples));
}

}  // namespace voice
}  // namespace tfrs
