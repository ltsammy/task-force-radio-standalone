// Parser for the legacy `"task_force_radio_pipe" callExtension "..."` protocol.
//
// The exact wire format (and every one of its quirks) is documented in
// docs/protocol-extension-legacy.md; addon/addons/ is an unmodified fork of
// the original SQF and must keep working byte for byte.
#pragma once

#include <string>

namespace tfrs {

class State;
class PipeClient;

class CommandProcessor {
public:
    CommandProcessor(State& state, PipeClient& pipe) : m_state(state), m_pipe(pipe) {}

    // Returns the exact string that has to be handed back to Arma.
    std::string process(const std::string& input);

private:
    std::string processSync(const std::string& command, const std::string& payload);
    void processAsync(const std::string& command, const std::string& payload);

    State& m_state;
    PipeClient& m_pipe;
};

}  // namespace tfrs
