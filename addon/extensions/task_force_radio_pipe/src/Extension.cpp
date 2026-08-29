// Arma 3 extension entry point.
//
// The exported signature is identical to the original
// old/extensions/task_force_radio_pipe/task_force_radio_pipe.cpp (which does
// NOT export RVExtensionVersion / RVExtensionArgs, so neither do we).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "CommandProcessor.h"
#include "State.h"
#include "Voice/VoiceSession.h"

namespace {

// ~15 Hz snapshot rate -- unchanged from the retired bridge protocol's own cadence, still well
// inside "every 50-100 ms" for radio-transmit-relay responsiveness.
const DWORD kSnapshotIntervalMs = 66;

// Everything is heap allocated and deliberately never freed: running static
// destructors (or joining threads) while the loader lock is held during
// DLL_PROCESS_DETACH is a classic source of shutdown deadlocks.
tfrs::State* g_state = nullptr;
tfrs::CommandProcessor* g_processor = nullptr;
std::thread* g_senderThread = nullptr;
// Native voice port (mic capture, Opus, UDP networking to Tfrs.VoiceServer, playback/mixing) --
// see the voice port plan doc. Owns its own threads; senderMain() below is the only place this
// module and State ever talk to each other.
tfrs::voice::VoiceSession* g_voice = nullptr;

std::once_flag g_initFlag;
std::atomic<bool> g_senderRunning(false);
std::atomic<bool> g_shuttingDown(false);
std::atomic<bool> g_ready(false);

// Maps State's `fx` string convention (see subtypeToFx/AudibleUnit in State.cpp) onto Voice/'s
// SourceEffect enum. Lives here, not in State.h or Voice/, because this file is the one place
// that's allowed to depend on both.
tfrs::voice::SourceEffect parseSourceEffect(const char* fx) {
    using tfrs::voice::SourceEffect;
    if (std::strcmp(fx, "sw") == 0) return SourceEffect::Sw;
    if (std::strcmp(fx, "lr") == 0) return SourceEffect::Lr;
    if (std::strcmp(fx, "airborne") == 0) return SourceEffect::Airborne;
    if (std::strcmp(fx, "dd") == 0) return SourceEffect::Dd;
    if (std::strcmp(fx, "phone") == 0) return SourceEffect::Phone;
    if (std::strcmp(fx, "speaker") == 0) return SourceEffect::Speaker;
    if (std::strcmp(fx, "intercom") == 0) return SourceEffect::Intercom;
    return SourceEffect::Direct;  // "direct", or anything unrecognized
}

// Matches the numeric values TFAR_Voice_TransmitMode's CBA LIST setting is defined with in
// fnc_initCBASettings.sqf ([[0, 1, 2], [...], 1]).
tfrs::voice::TransmitMode parseTransmitMode(float value) {
    using tfrs::voice::TransmitMode;
    if (value < 0.5f) return TransmitMode::PushToTalk;
    if (value < 1.5f) return TransmitMode::VoiceActivation;
    return TransmitMode::AlwaysOn;
}

// voice_serverPort comes from a free-text EDITBOX (unlike the SLIDER/LIST settings above, which
// can't produce anything but a well-formed in-range number from their own UI) -- garbage input
// (empty, "abc", "nan", a huge number) is a real possibility, and static_cast<uint16_t> of a NaN
// or out-of-range float is undefined behavior, not just "the wrong value". Clamping NaN to 0 is
// deliberate: VoiceNetworkClient::threadMain already treats port 0 as "not configured yet, wait"
// rather than attempting a connection at all, which is exactly the right behavior for garbage
// input too.
uint16_t parsePort(float value) {
    if (!(value >= 0.0f && value <= 65535.0f)) return 0;  // also catches NaN (fails every comparison)
    return static_cast<uint16_t>(value);
}

void senderMain() {
    while (g_senderRunning.load()) {
        g_state->setVoiceConnected(g_voice->isConnected());

        // CBA settings (fnc_initCBASettings.sqf's TFAR_Voice_* settings, relayed via SETCFG same
        // as every other TFAR setting) -> VoiceSession. Cheap enough to just re-apply every tick
        // rather than diffing for a change; setServer in particular relies on this: it's how a
        // live host/port/password edit gets picked up and triggers an automatic reconnect (see
        // VoiceNetworkClient::threadMain's configChangedLocked check).
        g_voice->setServer(g_state->voiceConfigString("voice_serverHost", ""),
                           parsePort(g_state->voiceConfigFloat("voice_serverPort", 9987.0f)),
                           g_state->voiceConfigString("voice_serverPassword", ""));
        g_voice->setMicVolume(g_state->voiceConfigFloat("voice_micVolume", 1.0f));
        g_voice->setSpeakerVolume(g_state->voiceConfigFloat("voice_speakerVolume", 1.0f));
        g_voice->setVadThreshold(g_state->voiceConfigFloat("voice_vadThreshold", 0.01f));
        g_voice->setTransmitMode(parseTransmitMode(g_state->voiceConfigFloat("voice_transmitMode", 1.0f)));

        g_state->setLocalTransmitting(g_voice->isTransmitting());
        // Idempotent to call every tick even once resolved -- VoiceNetworkClient's setIdentity is
        // a cheap mutex-guarded assignment, and this is what lets the placeholder identity
        // VoiceSession::start() seeds itself with get superseded the moment getPlayerUID resolves.
        g_voice->setIdentity(g_state->myUid(), g_state->myNickname());

        // Radio-transmit relay: what WE'RE sending (unconditionally every tick -- see
        // VoiceSession::sendRadioTx's doc comment) and what we've learned OTHERS are sending
        // (feeds back into State's audibility solver via setRemoteTx).
        const tfrs::State::LocalTxState localTx = g_state->localTxState();
        const uint16_t clampedRange =
            static_cast<uint16_t>(std::clamp(localTx.range, 0.0f, 65535.0f));
        g_voice->sendRadioTx(localTx.active, localTx.freq, clampedRange, localTx.subtype);
        for (const tfrs::voice::RadioTxInfo& tx : g_voice->currentRadioTx()) {
            g_state->setRemoteTx(tx.uid, tx.active, tx.freq, static_cast<float>(tx.range), tx.subtype);
        }

        // Local radio start/stop beep: edge-triggered off localTx.active. localTxState() only
        // reports current state (not a "changed" event like takeTransmitOverride below), and only
        // populates `subtype` while active (State.cpp's localTxState()) -- remember the last-seen
        // subtype so the end-of-transmission beep still knows which radio family it was.
        static bool s_lastLocalTxActive = false;
        static std::string s_lastLocalTxSubtype;
        if (localTx.active != s_lastLocalTxActive) {
            g_voice->triggerLocalBeep(localTx.active ? localTx.subtype : s_lastLocalTxSubtype,
                                      localTx.active);
            s_lastLocalTxActive = localTx.active;
        }
        if (localTx.active) s_lastLocalTxSubtype = localTx.subtype;

        // Transmit override: no pending change -> nothing to do; a change to "no override" means
        // normal PTT/VAD/AlwaysOn gating applies again; a change to true/false forces/blocks it.
        const std::optional<std::optional<bool>> overrideChange = g_state->takeTransmitOverride();
        if (overrideChange.has_value()) {
            const std::optional<bool>& newOverride = *overrideChange;
            g_voice->setAddonOverride(newOverride.has_value(), newOverride.value_or(false));
        }

        // The audibility solver's per-tick output, translated and pushed into playback.
        const std::vector<tfrs::AudibleUnit> units = g_state->computeAudibleUnits();
        std::vector<tfrs::voice::AudibilityUpdate> updates;
        updates.reserve(units.size());
        for (const tfrs::AudibleUnit& unit : units) {
            updates.push_back(tfrs::voice::AudibilityUpdate{
                unit.uid, unit.gain, unit.az, unit.muted, parseSourceEffect(unit.fx), unit.err});
        }
        g_voice->applyAudibility(updates);

        Sleep(kSnapshotIntervalMs);
    }
}

void initialize() {
    g_state = new tfrs::State();
    g_voice = new tfrs::voice::VoiceSession();
    g_processor = new tfrs::CommandProcessor(*g_state, *g_voice);

    g_voice->start();
    g_senderRunning.store(true);
    g_senderThread = new std::thread(senderMain);

    g_ready.store(true);
}

void ensureStarted() {
    // Never called from DllMain: creating threads under the loader lock
    // deadlocks. The first callExtension from SQF bootstraps everything.
    std::call_once(g_initFlag, initialize);
}

}  // namespace

extern "C" {

__declspec(dllexport) void __stdcall RVExtension(char* output, int outputSize,
                                                 const char* function) {
    if (output == nullptr || outputSize <= 0) return;
    output[0] = '\0';
    if (function == nullptr || function[0] == '\0') return;
    if (g_shuttingDown.load()) return;

    try {
        ensureStarted();
        if (!g_ready.load()) return;
        const std::string answer = g_processor->process(std::string(function));
        if (!answer.empty()) {
            strncpy_s(output, static_cast<size_t>(outputSize), answer.c_str(), _TRUNCATE);
        }
    } catch (...) {
        // Never let an exception escape into the game's main thread.
        output[0] = '\0';
    }
    output[outputSize - 1] = '\0';
}

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            break;
        case DLL_PROCESS_DETACH:
            // Only flag the shutdown -- for g_senderThread same as always, and now also for
            // g_voice's own threads (VoiceNetworkClient/WasapiCaptureEngine/PlaybackMixer), none
            // of which are joined here on purpose. Joining inside DllMain would deadlock on the
            // loader lock; Arma never unloads the extension so the OS reclaims everything (open
            // sockets, WASAPI handles, all of it) at process exit regardless.
            g_shuttingDown.store(true);
            g_senderRunning.store(false);
            break;
        default:
            break;
    }
    return TRUE;
}
