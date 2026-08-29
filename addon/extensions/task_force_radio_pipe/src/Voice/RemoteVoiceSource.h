// Per-remote-session playback: jitter-buffers and decodes queued Opus frames, applies the radio-
// effect chain + panning, and produces mixed-ready stereo samples on demand. Structural port of
// voice-client/src/Tfrs.VoiceClient/Audio/RemoteVoiceSource.cs.
//
// Real per-unit gain/azimuth/effect/err still arrives from State's solver only starting Phase 4;
// until then callers (Phase 3's VoiceSession) feed it directly.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Dsp/RadioEffectChain.h"
#include "OpusCodec.h"

namespace tfrs {
namespace voice {

// Mirrors RemoteSourceState in the C# reference.
struct RemoteSourceState {
    float gain = 0.0f;
    float azimuthRadians = 0.0f;
    bool muted = false;
    SourceEffect effect = SourceEffect::Direct;
    float errorLevel = 0.0f;

    static RemoteSourceState silent() { return RemoteSourceState{0.0f, 0.0f, true, SourceEffect::Direct, 0.0f}; }
};

class RemoteVoiceSource {
public:
    RemoteVoiceSource(uint32_t sessionId, std::string uid);

    uint32_t sessionId() const { return m_sessionId; }
    const std::string& uid() const { return m_uid; }

    // Called from the network receive thread as VoiceDown packets arrive for this session.
    void enqueueOpusFrame(const uint8_t* opus, size_t opusLen);

    // Called from the extension's main tick thread (Phase 4: with State's computed audibility).
    void setState(const RemoteSourceState& state);

    // Pulled from the render callback thread: writes exactly `frameCount` stereo interleaved
    // float samples (48kHz) into `out` (capacity >= frameCount*2). Never blocks.
    void render(float* out, size_t frameCount);

private:
    bool tryProduceNextFrame();
    void ensureEffectChain(SourceEffect effect);

    static constexpr int kJitterTargetFrames = 2;    // ~40ms buffered before playback starts
    static constexpr int kMaxConcealmentFrames = 5;  // ~100ms of PLC before going silent
    static constexpr size_t kMaxQueuedFrames = 10;   // bounds stall latency

    const uint32_t m_sessionId;
    const std::string m_uid;

    std::mutex m_queueMutex;
    std::deque<std::vector<uint8_t>> m_pending;

    // Render-thread-only (single caller: the WASAPI render callback) -- no synchronization needed.
    OpusVoiceDecoder m_decoder;
    std::vector<int16_t> m_decodedShorts;  // OpusFormat::kFrameSamples
    std::vector<float> m_monoFrame;        // OpusFormat::kFrameSamples, decoded PCM as float
    std::vector<float> m_stereoFrame;      // OpusFormat::kFrameSamples * 2
    size_t m_stereoFramePos = 0;           // >= m_stereoFrame.size() forces a decode on first render()
    bool m_isPlaying = false;
    int m_concealmentCount = 0;
    // Fully reconstructed whenever the effect type changes -- each effect owns its own filter/
    // delay/phase state that must never be shared or reset mid-talkspurt (matches the C#
    // reference's EnsureEffectChain).
    std::unique_ptr<RadioEffectChain> m_effectChain;
    SourceEffect m_currentEffect = SourceEffect::Direct;

    // Written from the main tick thread, read from the render thread.
    std::mutex m_stateMutex;
    RemoteSourceState m_state = RemoteSourceState::silent();
};

}  // namespace voice
}  // namespace tfrs
