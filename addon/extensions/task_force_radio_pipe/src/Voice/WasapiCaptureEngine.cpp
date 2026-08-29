#include "WasapiCaptureEngine.h"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <cstring>

#include "AudioDeviceUtil.h"
#include "OpusCodec.h"

namespace tfrs {
namespace voice {

namespace {

constexpr uint32_t kTargetSampleRate = OpusFormat::kSampleRate;
constexpr size_t kTargetFrameSamples = static_cast<size_t>(OpusFormat::kFrameSamples);

}  // namespace

WasapiCaptureEngine::WasapiCaptureEngine() = default;

WasapiCaptureEngine::~WasapiCaptureEngine() {
    stop();
}

void WasapiCaptureEngine::start(FrameCallback callback) {
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true)) return;
    m_callback = std::move(callback);
    m_stopRequested.store(false);
    m_thread = std::thread(&WasapiCaptureEngine::threadMain, this);
}

void WasapiCaptureEngine::stop() {
    if (!m_running.load()) return;
    m_stopRequested.store(true);
    if (m_thread.joinable()) m_thread.join();
    m_running.store(false);
}

bool WasapiCaptureEngine::isDeviceMutedAtOsLevel() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool weInitialized = (hr == S_OK || hr == S_FALSE);

    IMMDevice* device = AudioDeviceUtil::getDefaultDevice(AudioFlow::Capture);
    const bool muted = AudioDeviceUtil::isMutedOrZeroVolume(device);
    if (device != nullptr) device->Release();

    if (weInitialized) CoUninitialize();
    return muted;
}

void WasapiCaptureEngine::appendNativeSamples(const float* interleaved, uint32_t frameCount,
                                              uint16_t channels) {
    const size_t oldSize = m_nativeMono.size();
    m_nativeMono.resize(oldSize + frameCount);
    if (channels <= 1) {
        std::memcpy(m_nativeMono.data() + oldSize, interleaved, frameCount * sizeof(float));
        return;
    }
    for (uint32_t i = 0; i < frameCount; ++i) {
        float sum = 0.0f;
        for (uint16_t c = 0; c < channels; ++c) sum += interleaved[static_cast<size_t>(i) * channels + c];
        m_nativeMono[oldSize + i] = sum / static_cast<float>(channels);
    }
}

void WasapiCaptureEngine::resampleAndEmit(uint32_t nativeSampleRate) {
    if (nativeSampleRate == 0 || m_nativeMono.empty()) return;

    // Linear interpolation resample (see the header's doc comment for why not windowed-sinc). A
    // continuous fractional cursor into m_nativeMono means this handles nativeSampleRate ==
    // 48000 as a pure passthrough (step == 1.0) with no special-casing.
    const double step = static_cast<double>(nativeSampleRate) / static_cast<double>(kTargetSampleRate);

    for (;;) {
        const size_t idx0 = static_cast<size_t>(m_resampleCursor);
        const size_t idx1 = idx0 + 1;
        if (idx1 >= m_nativeMono.size()) break;  // need more native samples to interpolate further

        const double frac = m_resampleCursor - static_cast<double>(idx0);
        const float sample = static_cast<float>(m_nativeMono[idx0] * (1.0 - frac) +
                                                 m_nativeMono[idx1] * frac);
        m_frameBuffer.push_back(sample);
        m_resampleCursor += step;

        if (m_frameBuffer.size() >= kTargetFrameSamples) {
            if (m_callback) m_callback(m_frameBuffer.data());
            m_frameBuffer.erase(m_frameBuffer.begin(),
                                m_frameBuffer.begin() + static_cast<ptrdiff_t>(kTargetFrameSamples));
        }
    }

    // Drop fully-consumed native samples from the front so m_nativeMono doesn't grow unboundedly;
    // shift the cursor to match.
    const size_t consumed = static_cast<size_t>(m_resampleCursor);
    if (consumed > 0 && consumed <= m_nativeMono.size()) {
        m_nativeMono.erase(m_nativeMono.begin(), m_nativeMono.begin() + static_cast<ptrdiff_t>(consumed));
        m_resampleCursor -= static_cast<double>(consumed);
    }
}

void WasapiCaptureEngine::threadMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDevice* device = AudioDeviceUtil::getDefaultDevice(AudioFlow::Capture);
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

    const uint32_t nativeSampleRate = mixFormat->nSamplesPerSec;
    const uint16_t nativeChannels = mixFormat->nChannels;
    const uint16_t bitsPerSample = mixFormat->wBitsPerSample;
    const bool isFloat = AudioDeviceUtil::isFloatFormat(mixFormat);

    constexpr REFERENCE_TIME kBufferDuration = 200000;  // 20ms in 100ns units
    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 kBufferDuration, 0, mixFormat, nullptr);
    CoTaskMemFree(mixFormat);
    mixFormat = nullptr;
    if (FAILED(hr)) {
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

    IAudioCaptureClient* captureClient = nullptr;
    hr = audioClient->GetService(__uuidof(IAudioCaptureClient),
                                 reinterpret_cast<void**>(&captureClient));
    if (FAILED(hr) || captureClient == nullptr) {
        CloseHandle(event);
        audioClient->Release();
        CoUninitialize();
        return;
    }

    hr = audioClient->Start();
    if (FAILED(hr)) {
        captureClient->Release();
        CloseHandle(event);
        audioClient->Release();
        CoUninitialize();
        return;
    }

    thread_local std::vector<float> convertScratch;

    while (!m_stopRequested.load()) {
        const DWORD waitResult = WaitForSingleObject(event, 200);
        if (waitResult != WAIT_OBJECT_0) continue;  // timeout -- just re-check the stop flag

        UINT32 packetLength = 0;
        while (SUCCEEDED(captureClient->GetNextPacketSize(&packetLength)) && packetLength > 0) {
            BYTE* data = nullptr;
            UINT32 framesAvailable = 0;
            DWORD flags = 0;
            hr = captureClient->GetBuffer(&data, &framesAvailable, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
                // A gap must still advance the resample position with real zeros, or timing
                // desyncs versus wall-clock -- feed synthesized silence through the normal path.
                convertScratch.assign(static_cast<size_t>(framesAvailable) * nativeChannels, 0.0f);
                appendNativeSamples(convertScratch.data(), framesAvailable, nativeChannels);
            } else if (isFloat) {
                appendNativeSamples(reinterpret_cast<const float*>(data), framesAvailable,
                                    nativeChannels);
            } else if (bitsPerSample == 16) {
                const size_t sampleCount = static_cast<size_t>(framesAvailable) * nativeChannels;
                convertScratch.resize(sampleCount);
                const auto* src = reinterpret_cast<const int16_t*>(data);
                for (size_t i = 0; i < sampleCount; ++i) convertScratch[i] = src[i] / 32768.0f;
                appendNativeSamples(convertScratch.data(), framesAvailable, nativeChannels);
            }
            // Any other format (rare for a shared-mode GetMixFormat result) is silently dropped:
            // no frames get produced from it rather than crashing on an exotic device.

            captureClient->ReleaseBuffer(framesAvailable);
        }

        resampleAndEmit(nativeSampleRate);
    }

    audioClient->Stop();
    captureClient->Release();
    CloseHandle(event);
    audioClient->Release();
    CoUninitialize();
}

}  // namespace voice
}  // namespace tfrs
