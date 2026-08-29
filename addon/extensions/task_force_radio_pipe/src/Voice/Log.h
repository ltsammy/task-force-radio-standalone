// Diagnostic-only logging for the native voice port, mirroring the exact pattern the retired
// PipeClient.cpp used (see its own git history) -- which is what actually made the addon-bridge
// divergence bug diagnosable earlier in this project's life. There is no in-game UI for this
// module at all (unlike the old standalone client's log window), so a log file is the only way
// any of this is ever visible.
#pragma once

#include <cstdint>
#include <string>

namespace tfrs {
namespace voice {

// Best-effort: any failure here must never affect actual voice functionality, so every error is
// swallowed. Safe from any thread -- opens, appends, and closes the file on every call rather than
// holding it open, since these are rare state-transition events, not a hot path.
void logLine(const std::string& message);

// Formats an HRESULT/error code as 8 uppercase hex digits, no "0x" prefix -- callers prepend their
// own "0x" so the surrounding log text reads naturally (e.g. "hr=0x8007000E").
std::string toHex(uint32_t value);

}  // namespace voice
}  // namespace tfrs
