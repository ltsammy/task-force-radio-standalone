#include "CommandProcessor.h"

#include <vector>

#include "PipeClient.h"
#include "State.h"
#include "Util.h"

namespace tfrs {

namespace {

// Kept verbatim on purpose: fnc_sendPlayerInfo.sqf matches on this exact text
// (GVAR(noTSNotConnectedHint)), see docs/protocol-extension-legacy.md.
const char* const kNotConnected = "Not connected to TeamSpeak";

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

    if (!m_pipe.isConnected()) return kNotConnected;

    if (!async) return processSync(command, payload);

    processAsync(command, payload);

    if (!command.empty() && command[0] == 'D') {  // DFRAME
        if (m_state.needsConfig()) return "NEEDCFG";
        return "OK";
    }
    if (!command.empty() && command[0] == 'M') {  // MISSIONEND -> empty string
        return std::string();
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

    // TRACK (telemetry) and collectDebugInfo (diagnostics) are intentional
    // no-ops; unknown async commands are silently ignored, exactly like the
    // original which still answered "OK".
}

}  // namespace tfrs
