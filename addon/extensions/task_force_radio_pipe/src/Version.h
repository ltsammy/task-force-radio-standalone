#pragma once

namespace tfrs {

// Reported through `TS_INFO<TAB>VERSION`. The SQF side only forwards this
// string to TFAR_fnc_getTeamspeakPluginVersion, nothing compares it, so the
// value is free to choose (docs/protocol-extension-legacy.md).
constexpr const char* kPluginVersion = "TFRS-1.0.0";

}  // namespace tfrs
