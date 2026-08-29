#include "PlaybackMixer.h"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "AudioDeviceUtil.h"
#include "OpusCodec.h"

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
    m_sources.emplace(sessionId, std::make_unique<RemoteVoiceSource>(sessionId, uid));
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
}

void PlaybackMixer::removeAllSources() {
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    m_sources.clear();
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

    const float volume = m_masterVolume.load();
    const bool muted = m_muted.load();
    for (float& sample : mix) {
        sample *= volume;
        // Soft-clip limiter: cheap, no lookahead/attack-release state -- closes the "summed mix
        // isn't re-clamped" gap (each source is already clamped pre-mix, the sum wasn't).
        sample = sample / (1.0f + std::fabs(sample));
        if (muted) sample = 0.0f;
    }

    m_mixed48k.insert(m_mixed48k.end(), mix.begin(), mix.end());
}

void PlaybackMixer::threadMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDevice* device = AudioDeviceUtil::getDefaultDevice(AudioFlow::Render);
    if (device == nullptr) {
        CoUninitialize();
        return;
    }

    IAudioClient* audioClient = nullptr;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&audioClient));
    device->Release();
    if (FAILED(hr) || audioClient == nullptr) {
        CoUninitialize();
        return;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = audioClient->GetMixFormat(&mixFormat);
    if (FAILED(hr) || mixFormat == nullptr) {
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
        audioClient->Release();
        CoUninitialize();
        return;
    }

    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) {
        audioClient->Release();
        CoUninitialize();
        return;
    }
    audioClient->SetEventHandle(event);

    IAudioRenderClient* renderClient = nullptr;
    hr = audioClient->GetService(__uuidof(IAudioRenderClient),
                                 reinterpret_cast<void**>(&renderClient));
    if (FAILED(hr) || renderClient == nullptr) {
        CloseHandle(event);
        audioClient->Release();
        CoUninitialize();
        return;
    }

    UINT32 bufferFrameCount = 0;
    audioClient->GetBufferSize(&bufferFrameCount);

    hr = audioClient->Start();
    if (FAILED(hr)) {
        renderClient->Release();
        CloseHandle(event);
        audioClient->Release();
        CoUninitialize();
        return;
    }

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
