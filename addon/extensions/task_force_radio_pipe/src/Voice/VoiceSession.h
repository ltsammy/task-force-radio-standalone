// Orchestrator: owns the network client, transmit controller, and playback mixer, wiring their
// callbacks together. Structural equivalent of VoiceSessionCoordinator.cs.
//
// Extension.cpp's tick is the sole integration point between this and the legacy State/
// CommandProcessor module -- neither src/Voice/ header nor State.h includes the other, so all
// input (applyAudibility, setAddonOverride, sendRadioTx, setIdentity) and output (isConnected,
// isTransmitting, currentRadioTx) here uses plain types or this file's own small structs, never
// State::AudibleUnit directly.
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Dsp/RadioEffectChain.h"
#include "PlaybackMixer.h"
#include "TransmitController.h"
#include "VoiceNetworkClient.h"

namespace tfrs {
namespace voice {

// One row of State's per-tick audibility solver output, translated into Voice/'s own vocabulary.
struct AudibilityUpdate {
    std::string uid;
    float gain = 0.0f;
    float azimuth = 0.0f;
    bool muted = false;
    SourceEffect effect = SourceEffect::Direct;
    float errorLevel = 0.0f;
};

// Mirrors State::RemoteTx / the old bridge's "tx" message, resolved to a uid instead of a
// nickname.
struct RadioTxInfo {
    std::string uid;
    bool active = false;
    std::string freq;
    uint16_t range = 0;
    std::string subtype;
};

class VoiceSession {
public:
    VoiceSession();
    ~VoiceSession();
    VoiceSession(const VoiceSession&) = delete;
    VoiceSession& operator=(const VoiceSession&) = delete;

    void start();
    void stop();

    // Live-updatable; picked up by the network worker's next iteration. Phase 6 drives these from
    // CBA settings (State's config store); until then start() seeds a hardcoded placeholder.
    void setServer(const std::string& host, uint16_t port, const std::string& password);
    void setIdentity(const std::string& uid, const std::string& name);

    bool isConnected() const { return m_network.isConnected(); }
    bool isTransmitting() const { return m_transmit.isTransmitting(); }

    // -- PTT / mic-mute / speaker-mute, driven by CBA keybinds (see fnc_initKeybinds.sqf's
    // MicPTT/MicMute/SpeakerMute actions) via CommandProcessor's MICPTT/MICMUTE/SPEAKERMUTE
    // commands. Toggle, not set, for the two mutes -- matches the existing ToggleHeadset keybind
    // idiom already used in this addon; safe here since callExtension is a direct synchronous
    // call, nothing can be dropped in transit the way a lost UDP packet could.
    void setPttHeld(bool held) { m_transmit.setPttHeld(held); }
    void toggleMicMute() { m_transmit.setMicMuted(!m_transmit.isMicMuted()); }
    void toggleSpeakerMute() { m_playback.setMuted(!m_playback.isMuted()); }

    // -- CBA settings passthrough (see fnc_initCBASettings.sqf's TFAR_Voice_* settings) ----------
    void setTransmitMode(TransmitMode mode) { m_transmit.setMode(mode); }
    void setMicVolume(float volume) { m_transmit.setMicVolume(volume); }
    void setSpeakerVolume(float volume) { m_playback.setMasterVolume(volume); }
    void setVadThreshold(float threshold) { m_transmit.setVadThreshold(threshold); }

    // -- per-tick inputs from Extension.cpp, driven by State ----------------

    // Applies State::computeAudibleUnits()'s output to matching playback sources (uid -> sessionId
    // via the roster; unresolvable uids -- not yet joined on the voice server -- are silently
    // skipped, matching VoiceSessionCoordinator.OnUnitsReceived's "not on the voice server (yet)"
    // case). Full-replace semantics: any session previously active but missing from this snapshot
    // goes silent, exactly like the C# reference.
    void applyAudibility(const std::vector<AudibilityUpdate>& units);
    // Mirrors AddonTransmitOverride: no override -> normal PTT/VAD/AlwaysOn gating applies.
    void setAddonOverride(bool hasOverride, bool overrideValue);
    // Fire-and-forget; safe (and expected) to call every tick unconditionally regardless of
    // whether anything changed -- the receiving side's 1.5s expiry depends on a steady refresh
    // stream to self-heal a single dropped UDP packet.
    void sendRadioTx(bool active, const std::string& freq, uint16_t range, const std::string& sub);

    // -- per-tick outputs to Extension.cpp, driven into State ----------------

    // Currently-known remote radio-tx state, already pruned to entries received within the last
    // 1.5s (matches State.cpp's own kTxExpiry) -- Extension.cpp's tick just forwards whatever's
    // here into State::setRemoteTx every tick; an entry that stops appearing here ages out on
    // State's side by itself, no explicit "stopped" push needed.
    std::vector<RadioTxInfo> currentRadioTx() const;

private:
    struct TxCacheEntry {
        RadioTxInfo info;
        std::chrono::steady_clock::time_point receivedAt;
    };

    VoiceNetworkClient m_network;
    TransmitController m_transmit;
    PlaybackMixer m_playback;

    // uid<->sessionId roster, written from the network thread (onRemoteJoined/onRemoteLeft), read
    // from Extension.cpp's tick thread (applyAudibility) -- needs its own lock, unlike everything
    // else here which is either already-synchronized (m_network/m_transmit/m_playback) or
    // tick-thread-only.
    mutable std::mutex m_rosterMutex;
    std::unordered_map<std::string, uint32_t> m_uidToSession;

    // Tick-thread-only (single caller: applyAudibility, always from Extension.cpp's tick) -- no
    // synchronization needed.
    std::vector<uint32_t> m_lastActiveSessionIds;

    // Written from the network thread (onRadioTx), read from the tick thread (currentRadioTx).
    mutable std::mutex m_txCacheMutex;
    std::unordered_map<uint32_t, TxCacheEntry> m_txCache;
};

}  // namespace voice
}  // namespace tfrs
