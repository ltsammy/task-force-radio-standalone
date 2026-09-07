// WASAPI default-endpoint resolution. No device picker (see the voice port plan's device-selection
// decision): always resolves the OS default communications-role device. Matches
// voice-client/src/Tfrs.VoiceClient/Audio/AudioDevices.cs's Resolve()-when-no-device-chosen path.
#pragma once

#include <string>

struct IMMDevice;

namespace tfrs {
namespace voice {

enum class AudioFlow { Capture, Render };

class AudioDeviceUtil {
public:
    // Returns a COM object with one AddRef the caller owns (must Release()), or nullptr if no
    // default device is available (no mic/speaker plugged in, or WASAPI itself unavailable).
    //
    // Deliberately eMultimedia, not eCommunications: eCommunications can silently point at a
    // different device via the legacy Recording/Playback Devices control panel's "default
    // communication device" setting, which looks indistinguishable from "not picking up my mic at
    // all" with no error anywhere -- see AudioDevices.cs's doc comment for the same reasoning.
    static IMMDevice* getDefaultDevice(AudioFlow flow);

    // The endpoint id string of whatever getDefaultDevice(flow) would return right now, or an
    // empty string if there is no default device. Cheap enough to poll every second or so, which
    // is how the capture/playback threads notice the user switching their default headset while
    // Arma is running: WASAPI does NOT tear down an already-initialized stream when the *default*
    // endpoint changes (only when the device itself disappears), so without this the audio keeps
    // going to/from the device that was default at startup, with no error anywhere.
    static std::wstring getDefaultDeviceId(AudioFlow flow);

    // Same, for an already-resolved device.
    static std::wstring getDeviceId(IMMDevice* device);

    // True if the endpoint is muted, or its volume is at/near zero, at the OS mixer level --
    // distinct from the mic *privacy* permission, and from this app's own mute state. Diagnostic
    // only; matches AudioDevices.GetEndpointVolumeState.
    static bool isMutedOrZeroVolume(IMMDevice* device);

    // True if a WAVEFORMATEX* (from IAudioClient::GetMixFormat, capture or render) describes an
    // IEEE-float sample format rather than integer PCM. `format` is a `const WAVEFORMATEX*` kept
    // opaque here (as `const void*`) so this header doesn't need <mmreg.h>/<audioclient.h>; the
    // .cpp casts it back. Shared by WasapiCaptureEngine and PlaybackMixer, which both need the
    // same format-sniffing logic.
    static bool isFloatFormat(const void* waveFormatEx);
};

}  // namespace voice
}  // namespace tfrs
