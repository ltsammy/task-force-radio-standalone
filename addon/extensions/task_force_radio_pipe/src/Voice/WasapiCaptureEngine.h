// Event-driven WASAPI shared-mode microphone capture. Structural port of
// voice-client/src/Tfrs.VoiceClient/Audio/MicCaptureService.cs: resolves the OS default capture
// device, downmixes to mono + resamples to 48kHz if the device's native format differs, and
// raises a callback once per complete 20ms/960-sample frame.
//
// The resampler is deliberately linear interpolation, not windowed-sinc (unlike the C#
// reference's WdlResamplingSampleProvider) -- lower quality, adequate for voice bandwidth, and a
// disclosed simplification rather than an accident (see the voice port plan's decision on this).
//
// No STA/message-pump thread is needed here (unlike the old client's StaWorker.cs): that was a
// NAudio/.NET COM-marshaling requirement, not a WASAPI one. This thread just calls
// CoInitializeEx(COINIT_MULTITHREADED) on itself.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace tfrs {
namespace voice {

class WasapiCaptureEngine {
public:
    // Raised on the capture callback thread with exactly 960 mono float samples (20ms @ 48kHz) --
    // must not block, matches the C# reference's FrameCaptured doc comment.
    using FrameCallback = std::function<void(const float* mono960)>;

    WasapiCaptureEngine();
    ~WasapiCaptureEngine();
    WasapiCaptureEngine(const WasapiCaptureEngine&) = delete;
    WasapiCaptureEngine& operator=(const WasapiCaptureEngine&) = delete;

    // Starts capture on a dedicated thread. No-op if already running.
    void start(FrameCallback callback);
    void stop();

    bool isRunning() const { return m_running.load(); }
    // True if the default capture device is muted/zero-volume at the OS mixer level right now --
    // distinct from this app's own mic-mute setting. Safe from any thread (opens its own short-
    // lived COM objects on the calling thread).
    static bool isDeviceMutedAtOsLevel();

private:
    void threadMain();
    void appendNativeSamples(const float* interleaved, uint32_t frameCount, uint16_t channels);
    void resampleAndEmit(uint32_t nativeSampleRate);

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::thread m_thread;
    FrameCallback m_callback;

    // Thread-owned resample/accumulation state -- only threadMain (and the functions it calls
    // directly) ever touches these, no synchronization needed.
    std::vector<float> m_nativeMono;   // downmixed, still at the device's native sample rate
    double m_resampleCursor = 0.0;     // fractional read position into m_nativeMono
    std::vector<float> m_frameBuffer;  // resampled to 48kHz, accumulating toward 960 samples
};

}  // namespace voice
}  // namespace tfrs
