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
    int stereoMode = 0;  // 0 = stereo, 1 = leftOnly, 2 = rightOnly -- hard ear cut, bypasses azimuth panning

    static RemoteSourceState silent() {
        return RemoteSourceState{0.0f, 0.0f, true, SourceEffect::Direct, 0.0f, 0};
    }
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

    // Called from the network thread (VoiceSession's onRadioTx, on a start/end edge). Queues a
    // one-shot overlay, mixed into the next render() calls at this source's current gain/azimuth
    // (not run through the radio effect chain -- the distinction between radio families is baked
    // into which clip plays, not applied as DSP). A new trigger replaces any still-playing one.
    void triggerBeep(const int16_t* samples, size_t count);

    // Pulled from the render callback thread: writes exactly `frameCount` stereo interleaved
    // float samples (48kHz) into `out` (capacity >= frameCount*2). Never blocks.
    void render(float* out, size_t frameCount);

private:
    bool tryProduceNextFrame();
    void ensureEffectChain(SourceEffect effect);

    // Live diagnostic evidence (extension.log, "exhausted PLC" lines) showed many simultaneous/
    // repeated exhaustions across sessions in real play -- PLC exhaustion specifically means
    // packets stopped arriving with no explicit end-of-talkspurt marker (a real "stopped talking"
    // is handled separately via VoiceUp's LastFrame flag, never touches this path), so this was
    // real audible cutout, not a logging false alarm. The original 40ms/100ms budgets were too
    // tight to absorb brief gaps -- doubled both. Still small enough to not add noticeably more
    // latency to when a talkspurt audibly starts.
    static constexpr int kJitterTargetFrames = 4;     // ~80ms buffered before playback starts
    static constexpr int kMaxConcealmentFrames = 10;  // ~200ms of PLC before going silent
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
    // Diagnostic-only (Log.h): counts how many times THIS session has run out of real packets
    // and had to fall back to PLC for the full kMaxConcealmentFrames before giving up and going
    // silent -- a live report of audible "buzzing" heard by some listeners but not others for the
    // same speaker (ruling out a bad mic) points at something per-listener like jitter/packet loss
    // specific to each client's own path to the relay, not the transmitted audio itself. Repeated
    // concealment-exhaustion for one session is the direct signature of that.
    int m_concealmentExhaustedCount = 0;
    // Fully reconstructed whenever the effect type changes -- each effect owns its own filter/
    // delay/phase state that must never be shared or reset mid-talkspurt (matches the C#
    // reference's EnsureEffectChain).
    std::unique_ptr<RadioEffectChain> m_effectChain;
    SourceEffect m_currentEffect = SourceEffect::Direct;

    // Written from the main tick thread, read from the render thread.
    std::mutex m_stateMutex;
    RemoteSourceState m_state = RemoteSourceState::silent();

    // Cross-thread handoff for triggerBeep() (called from the network thread) -> render thread.
    // A single pending slot, not a queue: beep clips are short (~150-400ms) and a fresh trigger
    // superseding a still-playing one is acceptable, unlike opus frames which must never drop.
    std::mutex m_beepMutex;
    const int16_t* m_pendingBeepSamples = nullptr;
    size_t m_pendingBeepCount = 0;

    // Render-thread-only playback cursor for the currently-mixing beep, if any.
    const int16_t* m_beepSamples = nullptr;
    size_t m_beepTotal = 0;
    size_t m_beepPos = 0;
};

}  // namespace voice
}  // namespace tfrs
