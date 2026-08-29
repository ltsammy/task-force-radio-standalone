// Orchestrator: owns the network client, transmit controller, and playback mixer, wiring their
// callbacks together. Structural equivalent of VoiceSessionCoordinator.cs.
//
// Phase 2 stub: hardcoded connect parameters and no addon integration yet -- Extension.cpp just
// constructs one of these and calls start() once at bootstrap, same pattern as its other globals.
// Phase 4 replaces the hardcoded values with State's SETCFG-driven config and wires the per-tick
// audibility solver output into setSourceState/sendRadioTx; Phase 5/6 wire PTT/mute keybinds and
// CBA settings into m_transmit's setters.
#pragma once

#include <cstdint>
#include <string>

#include "PlaybackMixer.h"
#include "TransmitController.h"
#include "VoiceNetworkClient.h"

namespace tfrs {
namespace voice {

class VoiceSession {
public:
    VoiceSession();
    ~VoiceSession();
    VoiceSession(const VoiceSession&) = delete;
    VoiceSession& operator=(const VoiceSession&) = delete;

    void start();
    void stop();

    // Live-updatable; picked up by the network worker's next iteration (Phase 6 drives these from
    // CBA settings). Phase 2: called once from start() with hardcoded placeholder values.
    void setServer(const std::string& host, uint16_t port, const std::string& password);
    void setIdentity(const std::string& uid, const std::string& name);

    bool isConnected() const { return m_network.isConnected(); }

private:
    VoiceNetworkClient m_network;
    TransmitController m_transmit;
    PlaybackMixer m_playback;
};

}  // namespace voice
}  // namespace tfrs
