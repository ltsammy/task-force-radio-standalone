#include "PlaybackMixer.h"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "AudioDeviceUtil.h"
#include "Log.h"
#include "OpusCodec.h"
#include "RadioBeepAssets.h"

namespace tfrs {
namespace voice {

namespace {
constexpr size_t kChunkFrames = static_cast<size_t>(OpusFormat::kFrameSamples);  // 960 @ 48kHz
constexpr uint32_t kInternalSampleRate = static_cast<uint32_t>(OpusFormat::kSampleRate);
}  // namespace

PlaybackMixer::PlaybackMixer() = default;

PlaybackMixer::~PlaybackMixer() {
    stop();
}

void PlaybackMixer::start() {
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true)) return;
    m_stopRequested.store(false);
    m_thread = std::thread(&PlaybackMixer::threadMain, this);
}

void PlaybackMixer::stop() {
    if (!m_running.load()) return;
    m_stopRequested.store(true);
    if (m_thread.joinable()) m_thread.join();
    m_running.store(false);
}

void PlaybackMixer::addSource(uint32_t sessionId, const std::string& uid) {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    if (m_sources.find(sessionId) != m_sources.end()) return;
    auto source = std::make_unique<RemoteVoiceSource>(sessionId, uid);
    if (m_debugForceAudible.load()) {
        // Matches PlaybackEngine.DebugForceAudible in the C# reference: full volume, centered, no
        // radio effect, set once at creation -- VoiceSession.applyAudibility() never touches a
        // source's state at all while this is on (see its own early return), so this is the only
        // place that sets it.
        source->setState(RemoteSourceState{1.0f, 0.0f, false, SourceEffect::Direct, 0.0f});
    }
    m_sources.emplace(sessionId, std::move(source));
    // Diagnostic-only: the voice connection deliberately survives Arma mission transitions, so
    // "left the server" doesn't necessarily mean zero remote sources -- this pins down whether
    // a reported audio issue lines up with an actually-active source, or with none at all.
    logLine("playback: source added, sessionId=" + std::to_string(sessionId) +
           " (active sources now " + std::to_string(m_sources.size()) + ")");
}

void PlaybackMixer::setSourceState(uint32_t sessionId, const RemoteSourceState& state) {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    const auto it = m_sources.find(sessionId);
    if (it != m_sources.end()) it->second->setState(state);
}

void PlaybackMixer::enqueueOpusFrame(uint32_t sessionId, const uint8_t* opus, size_t opusLen) {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    const auto it = m_sources.find(sessionId);
    if (it != m_sources.end()) it->second->enqueueOpusFrame(opus, opusLen);
}

void PlaybackMixer::removeSource(uint32_t sessionId) {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    m_sources.erase(sessionId);
    logLine("playback: source removed, sessionId=" + std::to_string(sessionId) +
           " (active sources now " + std::to_string(m_sources.size()) + ")");
}

void PlaybackMixer::removeAllSources() {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    const size_t hadCount = m_sources.size();
    m_sources.clear();
    if (hadCount > 0) {
        logLine("playback: all " + std::to_string(hadCount) + " source(s) removed (disconnected)");
    }
}

void PlaybackMixer::triggerRemoteBeep(uint32_t sessionId, const std::string& subtype, bool start) {
    const BeepClip* clip = findBeepClip(subtype, /*local=*/false, start);
    if (clip == nullptr) return;
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    const auto it = m_sources.find(sessionId);
    if (it != m_sources.end()) it->second->triggerBeep(clip->samples, clip->count);
}

void PlaybackMixer::triggerLocalBeep(const std::string& subtype, bool start) {
    const BeepClip* clip = findBeepClip(subtype, /*local=*/true, start);
    if (clip == nullptr) return;
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    m_localBeepSamples = clip->samples;
    m_localBeepTotal = clip->count;
    m_localBeepPos = 0;
}

void PlaybackMixer::setMasterVolume(float volume) {
    m_masterVolume.store(std::clamp(volume, 0.0f, 2.0f));
}

void PlaybackMixer::setMuted(bool muted) {
    m_muted.store(muted);
}

void PlaybackMixer::generateChunkLocked() {
    std::vector<float> mix(kChunkFrames * 2, 0.0f);
    std::vector<float> scratch(kChunkFrames * 2);

    for (auto& entry : m_sources) {
        entry.second->render(scratch.data(), kChunkFrames);
        for (size_t i = 0; i < mix.size(); ++i) mix[i] += scratch[i];
    }

    // Local (self-feedback) beep overlay: centered, plain gain -- no panning/positioning, unlike
    // the per-source remote beep in RemoteVoiceSource::render.
    if (m_localBeepPos < m_localBeepTotal) {
        const size_t toMix = std::min(kChunkFrames, m_localBeepTotal - m_localBeepPos);
        for (size_t i = 0; i < toMix; ++i) {
            const float s = m_localBeepSamples[m_localBeepPos + i] / 32768.0f;
            mix[i * 2] += s;
            mix[i * 2 + 1] += s;
        }
        m_localBeepPos += toMix;
    }

    const float volume = m_masterVolume.load();
    const bool muted = m_muted.load();
    for (float& sample : mix) {
        sample *= volume;
        // Soft-clip limiter: cheap, no lookahead/attack-release state -- closes the "summed mix
        // isn't re-clamped" gap (each source is already clamped pre-mix, the sum wasn't).
        sample = sample / (1.0f + std::fabs(sample));
        if (muted) sample = 0.0f;
    }

    // Diagnostic-only: a live report of audible "buzzing" persisting after leaving the mission
    // (zero RemoteVoiceSources -- no remote/network audio possible) and only stopping when Arma
    // fully closes. If this render path is somehow the source, a non-silent mix with no active
    // sources and no local beep playing is the direct signature of it; if it never fires despite
    // the buzz still happening, that rules this whole path out instead. Throttled to ~once/second
    // (this runs every 20ms) so a minutes-long occurrence doesn't flood the log.
    if (m_sources.empty() && m_localBeepPos >= m_localBeepTotal && !muted) {
        float peak = 0.0f;
        for (const float sample : mix) peak = std::max(peak, std::fabs(sample));
        if (peak > 0.001f && (++m_phantomAudioLogCounter % 50) == 1) {
            logLine("playback: non-silent mix (peak=" + std::to_string(peak) +
                   ") with zero active sources and no beep playing -- possible phantom audio");
        }
    } else {
        m_phantomAudioLogCounter = 0;
    }

    m_mixed48k.insert(m_mixed48k.end(), mix.begin(), mix.end());
}

void PlaybackMixer::threadMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDevice* device = AudioDeviceUtil::getDefaultDevice(AudioFlow::Render);
    if (device == nullptr) {
        logLine("playback: no default speaker/headset device available");
        CoUninitialize();
        return;
    }

    IAudioClient* audioClient = nullptr;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&audioClient));
    device->Release();
    if (FAILED(hr) || audioClient == nullptr) {
        logLine("playback: IAudioClient activation failed, hr=0x" + toHex(static_cast<uint32_t>(hr)));
        CoUninitialize();
        return;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = audioClient->GetMixFormat(&mixFormat);
    if (FAILED(hr) || mixFormat == nullptr) {
        logLine("playback: GetMixFormat failed, hr=0x" + toHex(static_cast<uint32_t>(hr)));
        audioClient->Release();
        CoUninitialize();
        return;
    }

    const uint32_t deviceSampleRate = mixFormat->nSamplesPerSec;
    const uint16_t deviceChannels = mixFormat->nChannels;
    const uint16_t bitsPerSample = mixFormat->wBitsPerSample;
    const bool isFloat = AudioDeviceUtil::isFloatFormat(mixFormat);
    // Every render-loop write path below only handles float or 16-bit PCM (virtually always what
    // a real shared-mode mix format is) -- bailing out here for anything else means silence
    // (no audio output at all) rather than submitting an uninitialized/stale WASAPI buffer to the
    // device on every callback, which could play as loud garbage.
    if (!isFloat && bitsPerSample != 16) {
        logLine("playback: unsupported device mix format (" + std::to_string(bitsPerSample) +
               "-bit, not float) -- giving up rather than risk garbage audio output");
        CoTaskMemFree(mixFormat);
        audioClient->Release();
        CoUninitialize();
        return;
    }

    constexpr REFERENCE_TIME kBufferDuration = 200000;  // 20ms in 100ns units
    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 kBufferDuration, 0, mixFormat, nullptr);
    CoTaskMemFree(mixFormat);
    mixFormat = nullptr;
    if (FAILED(hr) || deviceChannels == 0) {
        logLine("playback: IAudioClient::Initialize failed, hr=0x" + toHex(static_cast<uint32_t>(hr)));
        audioClient->Release();
        CoUninitialize();
        return;
    }

    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) {
        logLine("playback: CreateEventW failed, error=" + std::to_string(GetLastError()));
        audioClient->Release();
        CoUninitialize();
        return;
    }
    audioClient->SetEventHandle(event);

    IAudioRenderClient* renderClient = nullptr;
    hr = audioClient->GetService(__uuidof(IAudioRenderClient),
                                 reinterpret_cast<void**>(&renderClient));
    if (FAILED(hr) || renderClient == nullptr) {
        logLine("playback: GetService(IAudioRenderClient) failed, hr=0x" +
               toHex(static_cast<uint32_t>(hr)));
        CloseHandle(event);
        audioClient->Release();
        CoUninitialize();
        return;
    }

    UINT32 bufferFrameCount = 0;
    audioClient->GetBufferSize(&bufferFrameCount);

    hr = audioClient->Start();
    if (FAILED(hr)) {
        logLine("playback: IAudioClient::Start failed, hr=0x" + toHex(static_cast<uint32_t>(hr)));
        renderClient->Release();
        CloseHandle(event);
        audioClient->Release();
        CoUninitialize();
        return;
    }

    logLine("playback: started, device format " + std::to_string(deviceSampleRate) + "Hz/" +
           std::to_string(deviceChannels) + "ch/" +
           (isFloat ? std::string("float") : std::to_string(bitsPerSample) + "-bit PCM"));

    // Resample step for the internal 48kHz mix -> the device's actual native rate. On most
    // systems these already match (step == 1.0, pure passthrough); this is what makes it correct
    // when they don't, same reasoning as WasapiCaptureEngine's capture-side resampler.
    const double step = static_cast<double>(kInternalSampleRate) / static_cast<double>(deviceSampleRate);
    std::vector<float> deviceScratch;  // reused conversion buffer for the non-float device-format path

    while (!m_stopRequested.load()) {
        const DWORD waitResult = WaitForSingleObject(event, 200);
        if (waitResult != WAIT_OBJECT_0) continue;  // timeout -- just re-check the stop flag

        UINT32 paddingFrames = 0;
        if (FAILED(audioClient->GetCurrentPadding(&paddingFrames))) continue;
        const UINT32 framesNeeded = bufferFrameCount - paddingFrames;
        if (framesNeeded == 0) continue;

        BYTE* data = nullptr;
        hr = renderClient->GetBuffer(framesNeeded, &data);
        if (FAILED(hr) || data == nullptr) continue;

        {
            std::lock_guard<std::mutex> lock(m_sourcesMutex);

            // Top up m_mixed48k until the resampler has enough to satisfy framesNeeded device-rate
            // output frames, then interpolate directly into the WASAPI buffer.
            float* floatOut = isFloat ? reinterpret_cast<float*>(data) : nullptr;
            if (!isFloat && bitsPerSample == 16) deviceScratch.resize(static_cast<size_t>(framesNeeded) * 2);

            for (UINT32 f = 0; f < framesNeeded; ++f) {
                while (static_cast<size_t>(m_resampleCursor) + 1 >= m_mixed48k.size() / 2) {
                    generateChunkLocked();
                }

                const size_t idx0 = static_cast<size_t>(m_resampleCursor) * 2;
                const size_t idx1 = idx0 + 2;
                const double frac = m_resampleCursor - std::floor(m_resampleCursor);
                const float left = static_cast<float>(m_mixed48k[idx0] * (1.0 - frac) +
                                                       m_mixed48k[idx1] * frac);
                const float right = static_cast<float>(m_mixed48k[idx0 + 1] * (1.0 - frac) +
                                                        m_mixed48k[idx1 + 1] * frac);
                m_resampleCursor += step;

                if (isFloat) {
                    // Stride is deviceChannels, not 2: a >2-channel device (rare in shared mode,
                    // but possible, e.g. 5.1) would otherwise overlap consecutive frames.
                    const size_t base = static_cast<size_t>(f) * deviceChannels;
                    floatOut[base] = left;
                    if (deviceChannels > 1) floatOut[base + 1] = right;
                    for (uint16_t c = 2; c < deviceChannels; ++c) floatOut[base + c] = 0.0f;
                } else if (bitsPerSample == 16) {
                    deviceScratch[static_cast<size_t>(f) * 2] = left;
                    deviceScratch[static_cast<size_t>(f) * 2 + 1] = right;
                }
            }

            if (!isFloat && bitsPerSample == 16) {
                auto* pcmOut = reinterpret_cast<int16_t*>(data);
                for (UINT32 f = 0; f < framesNeeded; ++f) {
                    const float l = std::clamp(deviceScratch[static_cast<size_t>(f) * 2], -1.0f, 1.0f);
                    const float r = std::clamp(deviceScratch[static_cast<size_t>(f) * 2 + 1], -1.0f, 1.0f);
                    pcmOut[static_cast<size_t>(f) * deviceChannels] = static_cast<int16_t>(l * 32767.0f);
                    if (deviceChannels > 1)
                        pcmOut[static_cast<size_t>(f) * deviceChannels + 1] = static_cast<int16_t>(r * 32767.0f);
                    for (uint16_t c = 2; c < deviceChannels; ++c)
                        pcmOut[static_cast<size_t>(f) * deviceChannels + c] = 0;
                }
            }

            // Drop fully-consumed 48kHz samples from the front so m_mixed48k doesn't grow
            // unboundedly, shifting the cursor to match.
            const size_t consumedFrames = static_cast<size_t>(m_resampleCursor);
            if (consumedFrames > 0 && consumedFrames * 2 <= m_mixed48k.size()) {
                m_mixed48k.erase(m_mixed48k.begin(),
                                 m_mixed48k.begin() + static_cast<ptrdiff_t>(consumedFrames * 2));
                m_resampleCursor -= static_cast<double>(consumedFrames);
            }
        }

        renderClient->ReleaseBuffer(framesNeeded, 0);
    }

    audioClient->Stop();
    renderClient->Release();
    CloseHandle(event);
    audioClient->Release();
    CoUninitialize();
}

}  // namespace voice
}  // namespace tfrs
