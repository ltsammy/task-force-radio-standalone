// Combines mic capture with AGC, VAD/PTT/AlwaysOn transmit gating, and Opus encode+send. Wire
// port of voice-client/src/Tfrs.VoiceClient/Audio/TransmitController.cs.
//
// Phase 2: mode/PTT/mute/override are all plain setters here, driven by hardcoded defaults from
// VoiceSession; Phase 5 wires PTT/mute to CBA keybinds, Phase 6 wires mode/threshold/volume to
// CBA settings, Phase 4 wires the addon override to State's transmitOverride.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Dsp/NoiseSuppressor.h"
#include "OpusCodec.h"
#include "WasapiCaptureEngine.h"

namespace tfrs {
namespace voice {

enum class TransmitMode { PushToTalk, VoiceActivation, AlwaysOn };

class TransmitController {
public:
    // Called on the capture callback thread with an encoded Opus frame ready to send -- must not
    // block, same constraint as WasapiCaptureEngine::FrameCallback.
    using SendCallback = std::function<void(const uint8_t* opus, size_t len, bool isLast)>;

    TransmitController();

    void start(SendCallback onSend);
    void stop();

    void setMode(TransmitMode mode) { m_mode.store(mode); }
    void setPttHeld(bool held) { m_pttHeld.store(held); }
    void setMicMuted(bool muted) { m_micMuted.store(muted); }
    bool isMicMuted() const { return m_micMuted.load(); }
    void setVadThreshold(float threshold) { m_vadThreshold.store(threshold); }
    void setMicVolume(float volume) { m_micVolume.store(volume); }
    // Applied before AGC/VAD/encoding, so a cleaner signal also improves VAD accuracy and AGC's
    // RMS measurement, not just what gets sent. RNNoise stays constructed either way (its own
    // init is cheap); disabling this just skips calling it, at zero runtime cost when off.
    void setNoiseSuppressionEnabled(bool enabled) { m_noiseSuppressionEnabled.store(enabled); }
    // Mirrors AddonTransmitOverride: no override -> normal gating; override=false blocks ALL
    // transmission (even AlwaysOn); override=true forces it regardless of mode.
    void setAddonOverride(bool hasOverride, bool overrideValue);

    bool isTransmitting() const { return m_isTransmitting.load(); }
    float currentLevel() const { return m_currentLevel.load(); }  // post-gain RMS, for a future VU hint

private:
    void onFrameCaptured(const float* mono960);
    bool determineShouldTransmit(float gainedRms);
    bool evaluateVoiceActivation(float gainedRms);

    WasapiCaptureEngine m_capture;
    OpusVoiceEncoder m_encoder;
    SendCallback m_onSend;

    std::atomic<TransmitMode> m_mode{TransmitMode::VoiceActivation};
    std::atomic<bool> m_pttHeld{false};
    std::atomic<bool> m_micMuted{false};
    std::atomic<bool> m_hasAddonOverride{false};
    std::atomic<bool> m_addonOverrideValue{false};
    std::atomic<float> m_vadThreshold{0.01f};
    std::atomic<float> m_micVolume{1.0f};
    std::atomic<bool> m_noiseSuppressionEnabled{true};
    std::atomic<float> m_currentLevel{0.0f};
    std::atomic<bool> m_isTransmitting{false};

    // Capture-callback-thread-only (single caller: WasapiCaptureEngine's own thread) -- no
    // synchronization needed for any of this.
    float m_autoGain = 1.0f;
    std::chrono::steady_clock::time_point m_vadHangoverUntil{};
    bool m_wasTransmitting = false;
    int m_silentFrameCount = 0;
    NoiseSuppressor m_noiseSuppressor;
    std::vector<float> m_denoiseScratch;
    std::vector<int16_t> m_pcmScratch;
    std::vector<uint8_t> m_opusScratch;

    // Diagnostic-only (Log.h): last-logged gating state, so onFrameCaptured (called ~50x/second)
    // only logs on an actual transition, not every frame.
    std::string m_lastLoggedGateState;
};

}  // namespace voice
}  // namespace tfrs
