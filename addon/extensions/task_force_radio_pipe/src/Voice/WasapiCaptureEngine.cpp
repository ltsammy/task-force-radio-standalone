#include "WasapiCaptureEngine.h"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <chrono>
#include <cstring>
#include <string>

#include "AudioDeviceUtil.h"
#include "Log.h"
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

    // Outer supervision loop. runDeviceSession() used to BE threadMain, so any failure inside it
    // -- no default device yet while Arma was still starting, activation failing while an audio
    // driver was still coming up, the endpoint being unplugged or invalidated later -- simply
    // returned and left the microphone permanently dead for the rest of the process, with nothing
    // short of restarting Arma able to bring it back. That is exactly the "hears everyone, nobody
    // hears me" shape of report. Now each of those cases just ends a session and a fresh one is
    // opened a second later.
    unsigned failureCount = 0;
    while (!m_stopRequested.load()) {
        const bool wasRunning = runDeviceSession();
        if (m_stopRequested.load()) break;

        if (wasRunning) {
            failureCount = 0;
            logLine("capture: device session ended (device lost or switched) -- reopening");
        } else if ((++failureCount % 30) == 1) {
            // Throttled: with no microphone present at all this repeats forever, ~once every 30s.
            logLine("capture: no usable microphone (attempt " + std::to_string(failureCount) +
                   ") -- retrying every second");
        }

        // Per-session accumulation state must not carry over into the next device, whose native
        // format may differ entirely.
        m_nativeMono.clear();
        m_frameBuffer.clear();
        m_resampleCursor = 0.0;

        for (int i = 0; i < 10 && !m_stopRequested.load(); ++i) Sleep(100);
    }

    CoUninitialize();
}

bool WasapiCaptureEngine::runDeviceSession() {
    IMMDevice* device = AudioDeviceUtil::getDefaultDevice(AudioFlow::Capture);
    if (device == nullptr) return false;
    const std::wstring deviceId = AudioDeviceUtil::getDeviceId(device);

    IAudioClient* audioClient = nullptr;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&audioClient));
    device->Release();
    if (FAILED(hr) || audioClient == nullptr) {
        logLine("capture: IAudioClient activation failed, hr=0x" + toHex(static_cast<uint32_t>(hr)));
        return false;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = audioClient->GetMixFormat(&mixFormat);
    if (FAILED(hr) || mixFormat == nullptr) {
        logLine("capture: GetMixFormat failed, hr=0x" + toHex(static_cast<uint32_t>(hr)));
        audioClient->Release();
        return false;
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
    if (FAILED(hr) || nativeChannels == 0) {
        logLine("capture: IAudioClient::Initialize failed, hr=0x" + toHex(static_cast<uint32_t>(hr)));
        audioClient->Release();
        return false;
    }

    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) {
        logLine("capture: CreateEventW failed, error=" + std::to_string(GetLastError()));
        audioClient->Release();
        return false;
    }
    audioClient->SetEventHandle(event);

    IAudioCaptureClient* captureClient = nullptr;
    hr = audioClient->GetService(__uuidof(IAudioCaptureClient),
                                 reinterpret_cast<void**>(&captureClient));
    if (FAILED(hr) || captureClient == nullptr) {
        logLine("capture: GetService(IAudioCaptureClient) failed, hr=0x" +
               toHex(static_cast<uint32_t>(hr)));
        CloseHandle(event);
        audioClient->Release();
        return false;
    }

    hr = audioClient->Start();
    if (FAILED(hr)) {
        logLine("capture: IAudioClient::Start failed, hr=0x" + toHex(static_cast<uint32_t>(hr)));
        captureClient->Release();
        CloseHandle(event);
        audioClient->Release();
        return false;
    }

    logLine("capture: started, native format " + std::to_string(nativeSampleRate) + "Hz/" +
           std::to_string(nativeChannels) + "ch/" +
           (isFloat ? std::string("float") : std::to_string(bitsPerSample) + "-bit PCM"));

    std::vector<float> convertScratch;
    // Polled rather than driven by an IMMNotificationClient: one string compare a second is far
    // less machinery than a COM callback object, and a second of latency on "the user just
    // switched their default microphone" is imperceptible next to the alternative (never noticing
    // -- WASAPI does not tear down an initialized stream when the *default* endpoint changes).
    auto lastDeviceCheck = std::chrono::steady_clock::now();

    while (!m_stopRequested.load()) {
        const DWORD waitResult = WaitForSingleObject(event, 200);
        if (waitResult == WAIT_OBJECT_0) {
            UINT32 packetLength = 0;
            bool deviceLost = false;
            while (SUCCEEDED(captureClient->GetNextPacketSize(&packetLength)) && packetLength > 0) {
                BYTE* data = nullptr;
                UINT32 framesAvailable = 0;
                DWORD flags = 0;
                hr = captureClient->GetBuffer(&data, &framesAvailable, &flags, nullptr, nullptr);
                if (FAILED(hr)) {
                    // AUDCLNT_E_DEVICE_INVALIDATED and friends: the endpoint is gone. This used
                    // to break out of the inner loop only, then spin on a dead client forever.
                    deviceLost = true;
                    break;
                }

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
                // Any other format (rare for a shared-mode GetMixFormat result) is silently
                // dropped: no frames get produced from it rather than crashing on an exotic device.

                captureClient->ReleaseBuffer(framesAvailable);
            }
            if (deviceLost) break;

            resampleAndEmit(nativeSampleRate);
        } else if (waitResult != WAIT_TIMEOUT) {
            break;  // the event handle itself went bad -- end the session and reopen
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - lastDeviceCheck >= std::chrono::seconds(1)) {
            lastDeviceCheck = now;
            const std::wstring currentDefault =
                AudioDeviceUtil::getDefaultDeviceId(AudioFlow::Capture);
            if (!currentDefault.empty() && !deviceId.empty() && currentDefault != deviceId) {
                logLine("capture: default microphone changed -- switching to the new device");
                break;
            }
        }
    }

    audioClient->Stop();
    captureClient->Release();
    CloseHandle(event);
    audioClient->Release();
    return true;
}

}  // namespace voice
}  // namespace tfrs
