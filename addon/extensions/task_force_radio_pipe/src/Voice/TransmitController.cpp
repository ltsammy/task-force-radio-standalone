#include "TransmitController.h"

#include <algorithm>
#include <cmath>

#include "Log.h"
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

const char* modeToString(TransmitMode mode) {
    switch (mode) {
        case TransmitMode::PushToTalk: return "PTT";
        case TransmitMode::VoiceActivation: return "VAD";
        case TransmitMode::AlwaysOn: return "AlwaysOn";
    }
    return "?";
}
}  // namespace

TransmitController::TransmitController() {
    m_denoiseScratch.resize(static_cast<size_t>(OpusFormat::kFrameSamples));
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

bool TransmitController::evaluateVoiceActivation(float level) {
    const auto now = std::chrono::steady_clock::now();
    if (level >= m_vadThreshold.load()) {
        m_vadHangoverUntil = now + kVadHangover;
        return true;
    }
    return now < m_vadHangoverUntil;
}

bool TransmitController::determineShouldTransmit(float level) {
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
            return evaluateVoiceActivation(level);
    }
    return false;
}

void TransmitController::onFrameCaptured(const float* rawMono960) {
    // Noise suppression runs first, before AGC/VAD/encoding, so a cleaner signal also improves
    // VAD accuracy and AGC's RMS measurement -- not just what ends up sent over the wire.
    const float* mono960 = rawMono960;
    if (m_noiseSuppressionEnabled.load()) {
        m_noiseSuppressor.process(rawMono960, m_denoiseScratch.data());
        mono960 = m_denoiseScratch.data();
    }

    double rawSumSquares = 0.0;
    for (int i = 0; i < OpusFormat::kFrameSamples; ++i) {
        rawSumSquares += static_cast<double>(mono960[i]) * mono960[i];
    }
    const float rawRms = static_cast<float>(std::sqrt(rawSumSquares / OpusFormat::kFrameSamples));

    // The level the VAD threshold is compared against: the mic signal at the level the USER set,
    // with AGC deliberately left out of it.
    //
    // This used to be measured AFTER AGC, which quietly defeated voice activation entirely: AGC
    // normalizes anything above ~0.0067 raw straight to its 0.08 target, so every input louder
    // than that landed 8x over the 0.01 default threshold, and even a whisper-quiet room got
    // multiplied by up to 12x. The effective raw gate worked out to ~0.0008 (-61 dBFS), i.e.
    // "any signal at all" -- so a VAD user was really running an always-open mic, transmitting
    // AGC-amplified room noise to everyone in direct-speech range the whole time.
    const float micVolume = m_micVolume.load();
    const float vadLevel = rawRms * micVolume;
    const bool levelIsVoiceLike = vadLevel >= m_vadThreshold.load();

    // AGC. Gain may always fall, but only rise while the signal actually looks like voice --
    // otherwise it winds all the way up to kAgcMaxGain during silence, and the first ~100ms of
    // every talkspurt (before the fast attack pulls it back down) blasts through at up to 12x,
    // which is heard as a click/crackle at the start of a transmission.
    if (rawRms >= 0.0001f) {
        const float desired = std::clamp(kAgcTargetRms / rawRms, kAgcMinGain, kAgcMaxGain);
        if (desired < m_autoGain) {
            m_autoGain += (desired - m_autoGain) * kAgcAttackRate;
        } else if (levelIsVoiceLike) {
            m_autoGain += (desired - m_autoGain) * kAgcReleaseRate;
        }
    }

    const float totalGain = micVolume * m_autoGain;
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

    const bool shouldTransmit = determineShouldTransmit(vadLevel);
    m_isTransmitting.store(shouldTransmit);

    // Diagnostic-only, edge-triggered on the DISCRETE state only (this runs ~50x/second;
    // gainedRms changes every frame, so keying the comparison on that too would defeat the
    // point and spam the log every single frame). Added specifically to pin down a live report
    // of PTT/mode-switching appearing to silently block transmission with no other symptom to
    // go on -- the continuous values are still included in the logged line for context, just
    // not part of what triggers logging it.
    {
        const std::string overrideStr = m_hasAddonOverride.load()
                                            ? (m_addonOverrideValue.load() ? "true" : "false")
                                            : "none";
        std::string triggerKey = std::string("mode=") + modeToString(m_mode.load()) +
                                 " pttHeld=" + (m_pttHeld.load() ? "1" : "0") +
                                 " micMuted=" + (m_micMuted.load() ? "1" : "0") +
                                 " override=" + overrideStr +
                                 " -> shouldTransmit=" + (shouldTransmit ? "1" : "0");
        if (triggerKey != m_lastLoggedGateState) {
            logLine("transmit-gate: " + triggerKey +
                   " (vadLevel=" + std::to_string(vadLevel) +
                   " vadThreshold=" + std::to_string(m_vadThreshold.load()) +
                   " gainedRms=" + std::to_string(gainedRms) +
                   " agc=" + std::to_string(m_autoGain) + ")");
            m_lastLoggedGateState = triggerKey;
        }
    }

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
