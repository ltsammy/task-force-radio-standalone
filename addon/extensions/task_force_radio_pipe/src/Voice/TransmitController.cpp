#include "TransmitController.h"

#include <algorithm>
#include <cmath>

#include "Protocol.h"

namespace tfrs {
namespace voice {

namespace {
constexpr float kAgcTargetRms = 0.08f;
constexpr float kAgcMinGain = 1.0f;
constexpr float kAgcMaxGain = 12.0f;
constexpr float kAgcAttackRate = 0.4f;   // fast, gain DOWN
constexpr float kAgcReleaseRate = 0.05f;  // slow, gain UP -- avoids audible "pumping"
constexpr auto kVadHangover = std::chrono::milliseconds(300);
}  // namespace

TransmitController::TransmitController() {
    m_pcmScratch.resize(static_cast<size_t>(OpusFormat::kFrameSamples));
    m_opusScratch.resize(kMaxOpusFrameLength);
}

void TransmitController::start(SendCallback onSend) {
    m_onSend = std::move(onSend);
    m_capture.start([this](const float* mono960) { onFrameCaptured(mono960); });
}

void TransmitController::stop() {
    m_capture.stop();
}

void TransmitController::setAddonOverride(bool hasOverride, bool overrideValue) {
    m_hasAddonOverride.store(hasOverride);
    m_addonOverrideValue.store(overrideValue);
}

bool TransmitController::evaluateVoiceActivation(float gainedRms) {
    const auto now = std::chrono::steady_clock::now();
    if (gainedRms >= m_vadThreshold.load()) {
        m_vadHangoverUntil = now + kVadHangover;
        return true;
    }
    return now < m_vadHangoverUntil;
}

bool TransmitController::determineShouldTransmit(float gainedRms) {
    if (m_hasAddonOverride.load()) {
        // false blocks everything, even AlwaysOn; true forces it regardless of mode.
        return m_addonOverrideValue.load();
    }
    if (m_micMuted.load()) return false;
    switch (m_mode.load()) {
        case TransmitMode::AlwaysOn:
            return true;
        case TransmitMode::PushToTalk:
            return m_pttHeld.load();
        case TransmitMode::VoiceActivation:
            return evaluateVoiceActivation(gainedRms);
    }
    return false;
}

void TransmitController::onFrameCaptured(const float* mono960) {
    double rawSumSquares = 0.0;
    for (int i = 0; i < OpusFormat::kFrameSamples; ++i) {
        rawSumSquares += static_cast<double>(mono960[i]) * mono960[i];
    }
    const float rawRms = static_cast<float>(std::sqrt(rawSumSquares / OpusFormat::kFrameSamples));

    // AGC: applied before VAD/threshold logic. Skipped entirely below this floor so it never
    // chases pure silence up to the target level.
    if (rawRms >= 0.0001f) {
        const float desired = std::clamp(kAgcTargetRms / rawRms, kAgcMinGain, kAgcMaxGain);
        const float rate = (desired < m_autoGain) ? kAgcAttackRate : kAgcReleaseRate;
        m_autoGain += (desired - m_autoGain) * rate;
    }

    const float totalGain = m_micVolume.load() * m_autoGain;
    double gainedSumSquares = 0.0;
    for (int i = 0; i < OpusFormat::kFrameSamples; ++i) {
        const float sample = std::clamp(mono960[i] * totalGain, -1.0f, 1.0f);
        gainedSumSquares += static_cast<double>(sample) * sample;
        m_pcmScratch[static_cast<size_t>(i)] =
            static_cast<int16_t>(std::clamp(sample * 32767.0f, -32768.0f, 32767.0f));
    }
    const float gainedRms = static_cast<float>(std::sqrt(gainedSumSquares / OpusFormat::kFrameSamples));
    m_currentLevel.store(gainedRms);

    // Persistent near-total silence (~3s) at the *gained* level is a proxy for the OS mic-privacy
    // permission silently zeroing capture -- matches the C# reference's heuristic (150 frames *
    // 20ms). Tracked but not yet surfaced anywhere; Phase 7's diagnostics pass wires this to a log
    // line / in-game hint.
    if (gainedRms < 0.0001f) {
        if (m_silentFrameCount < 1000000) ++m_silentFrameCount;
    } else {
        m_silentFrameCount = 0;
    }

    const bool shouldTransmit = determineShouldTransmit(gainedRms);
    m_isTransmitting.store(shouldTransmit);

    if (shouldTransmit) {
        const int encoded = m_encoder.encode(m_pcmScratch.data(), m_opusScratch.data(),
                                             static_cast<int>(m_opusScratch.size()));
        if (encoded > 0 && m_onSend) {
            m_onSend(m_opusScratch.data(), static_cast<size_t>(encoded), false);
        }
        m_wasTransmitting = true;
    } else if (m_wasTransmitting) {
        // Transmitting -> silent transition: one explicit end-of-talkspurt marker frame (literal
        // digital silence, flagged LastFrame), matching the C# reference.
        std::fill(m_pcmScratch.begin(), m_pcmScratch.end(), static_cast<int16_t>(0));
        const int encoded = m_encoder.encode(m_pcmScratch.data(), m_opusScratch.data(),
                                             static_cast<int>(m_opusScratch.size()));
        if (encoded > 0 && m_onSend) {
            m_onSend(m_opusScratch.data(), static_cast<size_t>(encoded), true);
        }
        m_wasTransmitting = false;
    }
}

}  // namespace voice
}  // namespace tfrs
