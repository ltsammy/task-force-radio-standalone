#include "CommandProcessor.h"

#include <vector>

#include "State.h"
#include "Util.h"
#include "Voice/VoiceSession.h"

namespace tfrs {

namespace {

// Kept verbatim on purpose: fnc_sendPlayerInfo.sqf matches on this exact text
// (GVAR(noTSNotConnectedHint)), see docs/protocol-extension-legacy.md. Wording is legacy --
// "TeamSpeak" hasn't been literally true since the original TS3-plugin architecture, and now
// means "not connected to the voice server" (src/Voice/VoiceNetworkClient), not an intermediate
// bridge -- but changing it would break that exact-text match on the SQF side.
const char* const kNotConnected = "Not connected to TeamSpeak";

// How long the voice connection has to have been down before POS starts reporting it. Connecting
// is not instant and never was: the server address only reaches the extension once SETCFG has
// relayed the CBA settings, DNS has to resolve, and the handshake itself retries up to 5x1.5s.
// Reporting from the very first POS meant every single mission start, and every brief reconnect
// after a server hiccup, flashed a "Not connected" popup at the player that had already fixed
// itself by the time they read it -- which is what made the message look random, and sent people
// looking for causes (a TeamSpeak that isn't running, an audio driver) that were never involved.
constexpr std::chrono::seconds kNotConnectedGrace(10);

std::string firstToken(const std::string& payload) {
    const size_t tab = payload.find('\t');
    if (tab == std::string::npos) return payload;
    return payload.substr(0, tab);
}

}  // namespace

std::string CommandProcessor::process(const std::string& input) {
    if (input.empty()) return std::string();

    // The trailing '~' is the one and only sync/async marker. It is stripped
    // before parsing so the last field never carries it (the original left it
    // in, which silently broke isTrue() on the last POS field).
    const bool async = (input.back() == '~');
    const std::string payload = async ? input.substr(0, input.size() - 1) : input;
    const std::string command = firstToken(payload);

    if (!async) return processSync(command, payload);

    processAsync(command, payload);

    if (command == "DFRAME") {
        if (m_state.needsConfig()) return "NEEDCFG";
        return "OK";
    }
    if (command == "MISSIONEND") {  // -> empty string
        return std::string();
    }
    // POS is the one command whose "not connected" reply drives fnc_sendPlayerInfo.sqf's hint
    // popup (docs/protocol-extension-legacy.md point 2). Every other async command (notably
    // SETCFG) must NOT be gated on this: m_voice.isConnected() is itself downstream of a
    // SETCFG-delivered host/port, so gating SETCFG on it would deadlock -- the address could
    // never be configured in the first place.
    //
    // Only reported once the outage has lasted kNotConnectedGrace, so normal startup and short
    // reconnects stay silent. Called from Arma's own thread only, so the timestamp needs no
    // synchronization.
    if (command == "POS") {
        if (m_voice.isConnected()) {
            m_disconnectedSince = std::chrono::steady_clock::time_point();
        } else {
            const auto now = std::chrono::steady_clock::now();
            if (m_disconnectedSince == std::chrono::steady_clock::time_point()) {
                m_disconnectedSince = now;
            } else if (now - m_disconnectedSince >= kNotConnectedGrace) {
                return kNotConnected;
            }
        }
    }
    return "OK";
}

std::string CommandProcessor::processSync(const std::string& command,
                                          const std::string& payload) {
    if (command == "TS_INFO") {
        const std::vector<std::string> tokens = splitLimit(payload, '\t', 2);
        if (tokens.size() < 2) return "FAIL";
        return m_state.tsInfo(tokens[1]);
    }

    if (command == "IS_SPEAKING") {
        const std::vector<std::string> tokens = splitLimit(payload, '\t', 2);
        if (tokens.size() < 2) return "00";
        return m_state.speakingPair(tokens[1]);
    }

    // Additive, not part of the legacy protocol -- polled by an SQF HUD indicator (voice port
    // Phase 5's MICMUTE/SPEAKERMUTE keybinds toggle the first two; nothing before now let SQF
    // read any of this back). Same shape as speakingPair: char 0 = mic muted, char 1 = speaker
    // muted, char 2 = currently transmitting (direct speech OR radio -- isTransmitting() reflects
    // TransmitController's actual gate decision, not just "PTT key held").
    if (command == "MUTE_STATE") {
        std::string result = "000";
        if (m_voice.isMicMuted()) result[0] = '1';
        if (m_voice.isSpeakerMuted()) result[1] = '1';
        if (m_voice.isTransmitting()) result[2] = '1';
        return result;
    }

    if (command == "IS_SPEAKING_BULK") {
        const std::vector<std::string> tokens = split(payload, '\t');
        std::string result;
        result.reserve(tokens.size() * 3);
        // One pair plus a tab per requested name, INCLUDING after the last one.
        for (size_t i = 1; i < tokens.size(); ++i) {
            result += m_state.speakingPair(tokens[i]);
            result += '\t';
        }
        return result;
    }

    return "UNKNOWN COMMAND";
}

void CommandProcessor::processAsync(const std::string& command, const std::string& payload) {
    if (command == "POS") {
        const std::vector<std::string> tokens = split(payload, '\t');
        m_state.handlePos(tokens);
        return;
    }

    if (command == "FREQ") {
        const std::vector<std::string> tokens = split(payload, '\t');
        m_state.handleFreq(tokens);
        return;
    }

    if (command == "TANGENT" || command == "TANGENT_LR" || command == "TANGENT_DD") {
        const std::vector<std::string> tokens = split(payload, '\t');
        m_state.handleTangent(tokens);
        return;
    }

    if (command == "SPEAKERS") {
        const std::vector<std::string> tokens = splitLimit(payload, '\t', 2);
        m_state.handleSpeakers(tokens);
        return;
    }

    if (command == "SETCFG") {
        const std::vector<std::string> tokens = splitLimit(payload, '\t', 4);
        m_state.handleSetCfg(tokens);
        return;
    }

    if (command == "KILLED") {
        const std::vector<std::string> tokens = splitLimit(payload, '\t', 2);
        if (tokens.size() >= 2) m_state.handleKilled(tokens[1]);
        return;
    }

    // Additive, not part of the legacy protocol (docs/protocol-extension-legacy.md) -- a new
    // one-time-per-unit call from fnc_sendPlayerInfo.sqf carrying that unit's real getPlayerUID.
    if (command == "UID") {
        const std::vector<std::string> tokens = splitLimit(payload, '\t', 3);
        if (tokens.size() >= 3) m_state.handleUid(tokens[1], tokens[2]);
        return;
    }

    if (command == "RELEASE_ALL_TANGENTS") {
        const std::vector<std::string> tokens = splitLimit(payload, '\t', 2);
        m_state.handleReleaseAllTangents(tokens.size() >= 2 ? tokens[1] : std::string());
        return;
    }

    if (command == "RadioTwrAdd") {
        const std::vector<std::string> tokens = splitLimit(payload, '\t', 2);
        if (tokens.size() >= 2) m_state.handleAddRadioTowers(tokens[1]);
        return;
    }

    if (command == "RadioTwrDel") {
        const std::vector<std::string> tokens = splitLimit(payload, '\t', 2);
        if (tokens.size() >= 2) m_state.handleDelRadioTowers(tokens[1]);
        return;
    }

    if (command == "DFRAME") {
        m_state.handleDataFrame();
        return;
    }

    if (command == "MISSIONEND") {
        m_state.handleMissionEnd();
        return;
    }

    // Additive, not part of the legacy protocol -- driven by fnc_initKeybinds.sqf's MicPTT/
    // MicMute/SpeakerMute cba_fnc_addKeybind actions (voice port Phase 5), replacing the old
    // standalone client's own GetAsyncKeyState polling with Arma's own input handling.
    if (command == "MICPTT") {
        const std::vector<std::string> tokens = splitLimit(payload, '\t', 2);
        if (tokens.size() >= 2) m_voice.setPttHeld(tokens[1] == "PRESSED");
        return;
    }

    if (command == "MICMUTE") {
        m_voice.toggleMicMute();
        return;
    }

    if (command == "SPEAKERMUTE") {
        m_voice.toggleSpeakerMute();
        return;
    }

    // TRACK (telemetry) and collectDebugInfo (diagnostics) are intentional
    // no-ops; unknown async commands are silently ignored, exactly like the
    // original which still answered "OK".
}

}  // namespace tfrs
