#include "RemoteVoiceSource.h"

#include <algorithm>
#include <cstring>

#include "Dsp/Panning.h"

namespace tfrs {
namespace voice {

RemoteVoiceSource::RemoteVoiceSource(uint32_t sessionId, std::string uid)
    : m_sessionId(sessionId), m_uid(std::move(uid)) {
    m_decodedShorts.resize(static_cast<size_t>(OpusFormat::kFrameSamples));
    m_monoFrame.resize(static_cast<size_t>(OpusFormat::kFrameSamples));
    m_stereoFrame.resize(static_cast<size_t>(OpusFormat::kFrameSamples) * 2);
    m_stereoFramePos = m_stereoFrame.size();  // force a decode on the first render()
    ensureEffectChain(SourceEffect::Direct);
}

void RemoteVoiceSource::ensureEffectChain(SourceEffect effect) {
    if (m_effectChain && effect == m_currentEffect) return;
    m_currentEffect = effect;
    m_effectChain = std::make_unique<RadioEffectChain>(effect);
}

void RemoteVoiceSource::enqueueOpusFrame(const uint8_t* opus, size_t opusLen) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_pending.emplace_back(opus, opus + opusLen);
    while (m_pending.size() > kMaxQueuedFrames) m_pending.pop_front();
}

void RemoteVoiceSource::setState(const RemoteSourceState& state) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_state = state;
}

bool RemoteVoiceSource::tryProduceNextFrame() {
    RemoteSourceState state;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        state = m_state;
    }

    if (state.muted) {
        m_isPlaying = false;
        std::lock_guard<std::mutex> lock(m_queueMutex);
        return m_pending.empty();  // drain quietly rather than leaving stale packets queued
    }

    std::vector<uint8_t> opus;
    bool hasPacket = false;
    bool notEnoughBuffered = false;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (!m_pending.empty()) {
            opus = std::move(m_pending.front());
            m_pending.pop_front();
            hasPacket = true;
            if (!m_isPlaying && static_cast<int>(m_pending.size()) < kJitterTargetFrames - 1) {
                // (Re)starting a talkspurt: wait for a short jitter cushion before audio begins,
                // to smooth out network jitter on the first few frames of a PTT press. Put the
                // packet back rather than drop it.
                m_pending.push_front(std::move(opus));
                notEnoughBuffered = true;
            }
        }
    }
    if (notEnoughBuffered) return false;

    if (!hasPacket) {
        if (!m_isPlaying) return false;  // nothing was playing -- stay silent, don't fabricate audio
        if (m_concealmentCount >= kMaxConcealmentFrames) {
            m_isPlaying = false;
            m_concealmentCount = 0;
            return false;
        }
        ++m_concealmentCount;
        m_decoder.decodePacketLoss(m_decodedShorts.data());
    } else {
        m_isPlaying = true;
        m_concealmentCount = 0;
        m_decoder.decode(opus.data(), static_cast<int>(opus.size()), m_decodedShorts.data());
    }

    for (size_t i = 0; i < m_decodedShorts.size(); ++i) m_monoFrame[i] = m_decodedShorts[i] / 32768.0f;

    ensureEffectChain(state.effect);
    m_effectChain->process(m_monoFrame.data(), m_monoFrame.size(), state.errorLevel);

    const auto [left, right] = Panning::compute(state.azimuthRadians);
    const float leftGain = left * state.gain;
    const float rightGain = right * state.gain;
    for (size_t i = 0; i < m_monoFrame.size(); ++i) {
        m_stereoFrame[i * 2] = std::clamp(m_monoFrame[i] * leftGain, -1.0f, 1.0f);
        m_stereoFrame[i * 2 + 1] = std::clamp(m_monoFrame[i] * rightGain, -1.0f, 1.0f);
    }
    return true;
}

void RemoteVoiceSource::triggerBeep(const int16_t* samples, size_t count) {
    std::lock_guard<std::mutex> lock(m_beepMutex);
    m_pendingBeepSamples = samples;
    m_pendingBeepCount = count;
}

void RemoteVoiceSource::render(float* out, size_t frameCount) {
    size_t written = 0;
    while (written < frameCount) {
        if (m_stereoFramePos >= m_stereoFrame.size()) {
            if (!tryProduceNextFrame()) {
                std::memset(out + written * 2, 0, (frameCount - written) * 2 * sizeof(float));
                written = frameCount;
                break;
            }
            m_stereoFramePos = 0;
        }

        const size_t availableFrames = (m_stereoFrame.size() - m_stereoFramePos) / 2;
        const size_t toCopy = std::min(availableFrames, frameCount - written);
        std::memcpy(out + written * 2, m_stereoFrame.data() + m_stereoFramePos,
                    toCopy * 2 * sizeof(float));
        m_stereoFramePos += toCopy * 2;
        written += toCopy;
    }

    // Pick up any pending beep trigger, then mix whatever's currently playing on top of the voice
    // output above (which may just be the silence written by the loop's memset branch -- a beep
    // should still play even if nobody's actively talking on the radio right now).
    {
        std::lock_guard<std::mutex> lock(m_beepMutex);
        if (m_pendingBeepSamples != nullptr) {
            m_beepSamples = m_pendingBeepSamples;
            m_beepTotal = m_pendingBeepCount;
            m_beepPos = 0;
            m_pendingBeepSamples = nullptr;
        }
    }

    if (m_beepPos >= m_beepTotal) return;

    RemoteSourceState state;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        state = m_state;
    }
    if (state.muted) return;  // matches tryProduceNextFrame's own muted gate

    const auto [left, right] = Panning::compute(state.azimuthRadians);
    const float leftGain = left * state.gain;
    const float rightGain = right * state.gain;
    const size_t toMix = std::min(frameCount, m_beepTotal - m_beepPos);
    for (size_t i = 0; i < toMix; ++i) {
        const float s = m_beepSamples[m_beepPos + i] / 32768.0f;
        out[i * 2] = std::clamp(out[i * 2] + s * leftGain, -1.0f, 1.0f);
        out[i * 2 + 1] = std::clamp(out[i * 2 + 1] + s * rightGain, -1.0f, 1.0f);
    }
    m_beepPos += toMix;
}

}  // namespace voice
}  // namespace tfrs
