// Event-driven WASAPI shared-mode playback: mixes every active RemoteVoiceSource (each internally
// producing 48kHz stereo), applies master volume + a mute gate + a soft-clip limiter, resamples to
// the render device's actual native rate, and renders it. Structural port of
// voice-client/src/Tfrs.VoiceClient/Audio/PlaybackEngine.cs.
//
// All source access is by session id through this class, never a raw pointer/reference handed to
// a caller: RemoteVoiceSource objects are owned by a std::unique_ptr in m_sources, and the render
// thread calls render() on them while holding the same m_sourcesMutex that guards add/remove --
// otherwise a concurrent removeSource() (from the network thread, on a ClientLeft) could destroy a
// source out from under the render thread mid-call.
//
// The limiter closes a real gap the C# reference had: each RemoteVoiceSource clamps its own
// samples pre-mix, but the *summed* mix was never re-clamped, so multiple loud simultaneous
// sources could clip (see docs/dsp-audio-pipeline.md section 7, which recommends at least this
// much). A soft-clip curve, not a full attack/release compressor -- the same doc calls a real
// compressor "a nice-to-have, not a correctness blocker".
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "RemoteVoiceSource.h"

namespace tfrs {
namespace voice {

class PlaybackMixer {
public:
    PlaybackMixer();
    ~PlaybackMixer();
    PlaybackMixer(const PlaybackMixer&) = delete;
    PlaybackMixer& operator=(const PlaybackMixer&) = delete;

    void start();
    void stop();

    // All no-ops for an unknown sessionId except addSource, which creates it if missing.
    void addSource(uint32_t sessionId, const std::string& uid);
    void setSourceState(uint32_t sessionId, const RemoteSourceState& state);
    void enqueueOpusFrame(uint32_t sessionId, const uint8_t* opus, size_t opusLen);
    void removeSource(uint32_t sessionId);
    void removeAllSources();

    // Radio start/stop "beep" cues (RadioBeepAssets.h). Remote: 3D-positioned at that session's
    // current gain/azimuth, via RemoteVoiceSource::triggerBeep. Local: centered self-feedback,
    // mixed directly into the master buffer since there's no per-source concept for "self".
    // subtype: the raw tf_subtype config value ("digital"/"digital_lr"/"airborne"/"dd") -- see
    // RadioBeepAssets.h. Anything else has no clip and is a silent no-op.
    void triggerRemoteBeep(uint32_t sessionId, const std::string& subtype, bool start);
    void triggerLocalBeep(const std::string& subtype, bool start);

    // 0..2, matches PlaybackEngine.MasterVolume's clamp range.
    void setMasterVolume(float volume);
    void setMuted(bool muted);
    bool isMuted() const { return m_muted.load(); }

    // Mirrors ServerOptions.DebugForceAudible: when true, newly added sources start fully
    // audible/centered/undistorted. Set by VoiceSession once ConnectAccept reports it.
    void setDebugForceAudible(bool enabled) { m_debugForceAudible.store(enabled); }
    bool debugForceAudible() const { return m_debugForceAudible.load(); }

private:
    void threadMain();
    // Mixes one more 960-sample (20ms) chunk from every active source into m_mixed48k, applying
    // master volume, mute gate, and the soft-clip limiter.
    void generateChunkLocked();

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::thread m_thread;

    std::atomic<float> m_masterVolume{1.0f};
    std::atomic<bool> m_muted{false};
    std::atomic<bool> m_debugForceAudible{false};

    std::mutex m_sourcesMutex;
    std::unordered_map<uint32_t, std::unique_ptr<RemoteVoiceSource>> m_sources;

    // Local beep playback cursor -- guarded by m_sourcesMutex (triggerLocalBeep from the tick
    // thread, consumed by generateChunkLocked under the same lock the render thread already holds
    // it under, same pattern as m_sources itself).
    const int16_t* m_localBeepSamples = nullptr;
    size_t m_localBeepTotal = 0;
    size_t m_localBeepPos = 0;

    // Render-thread-only (single caller: the WASAPI render callback) -- no synchronization needed.
    std::vector<float> m_mixed48k;    // interleaved stereo, accumulating ahead of the resampler
    double m_resampleCursor = 0.0;    // fractional read position into m_mixed48k, in STEREO FRAMES

    // Diagnostic-only (Log.h): a live report of audible "buzzing" that persists even after
    // leaving the mission (so with zero active RemoteVoiceSources, ruling out remote/network
    // audio) and only stops when Arma fully closes -- pointing at this render path rather than
    // anything connection-dependent. Throttled to roughly once/second while it's actually
    // happening, not per-chunk (this runs every 20ms; unthrottled, a minutes-long occurrence
    // would flood the log with thousands of lines).
    unsigned m_phantomAudioLogCounter = 0;
};

}  // namespace voice
}  // namespace tfrs
