#include "AudioDeviceUtil.h"

#include <windows.h>

#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <cstring>

namespace tfrs {
namespace voice {

namespace {

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, defined locally rather than pulling in <ksmedia.h> (which
// needs INITGUID linkage plumbing for a single well-known constant).
constexpr GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

}  // namespace

IMMDevice* AudioDeviceUtil::getDefaultDevice(AudioFlow flow) {
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || enumerator == nullptr) return nullptr;

    const EDataFlow edFlow = (flow == AudioFlow::Capture) ? eCapture : eRender;
    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(edFlow, eMultimedia, &device);
    enumerator->Release();
    if (FAILED(hr)) return nullptr;
    return device;
}

bool AudioDeviceUtil::isMutedOrZeroVolume(IMMDevice* device) {
    if (device == nullptr) return false;

    IAudioEndpointVolume* volume = nullptr;
    const HRESULT hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(&volume));
    if (FAILED(hr) || volume == nullptr) return false;

    BOOL muted = FALSE;
    float level = 1.0f;
    volume->GetMute(&muted);
    volume->GetMasterVolumeLevelScalar(&level);
    volume->Release();

    return muted != FALSE || level < 0.01f;
}

bool AudioDeviceUtil::isFloatFormat(const void* waveFormatEx) {
    const auto* format = static_cast<const WAVEFORMATEX*>(waveFormatEx);
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return std::memcmp(&ext->SubFormat, &kSubtypeIeeeFloat, sizeof(GUID)) == 0;
    }
    return false;
}

}  // namespace voice
}  // namespace tfrs
