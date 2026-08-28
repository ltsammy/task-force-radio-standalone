// COMPONENT should be defined in the script_component.hpp and included BEFORE this hpp

#define MAINPREFIX z
#define PREFIX tfar

#include "script_version.hpp"

#define VERSION MAJOR.MINOR.PATCHLVL.BUILD
#define VERSION_AR MAJOR,MINOR,PATCHLVL,BUILD
// CBA's default VERSION_CONFIG (script_macros_common.hpp) does `version = VERSION;` unquoted,
// which requires VERSION to be a single valid Arma number. VERSION here is a 4-part
// MAJOR.MINOR.PATCHLVL.BUILD expression (e.g. "1.-1.0.273"), which is not a valid single number —
// the original BI tooling silently tolerated the malformed value, HEMTT's stricter parser
// correctly rejects it. Override VERSION_CONFIG (before script_macros.hpp pulls in CBA's default,
// so CBA's #ifndef guard picks this up) so the legacy scalar `version` field gets a real single
// number (BUILD) while versionStr/versionAr keep the full, unabbreviated version info unchanged.
#define VERSION_CONFIG version = BUILD; versionStr = QUOTE(VERSION); versionAr[] = {VERSION_AR}
#define TFAR_ADDON_VERSION QUOTE(VERSION)
#define SERVER_API_VERSION 1

// MINIMAL required version for the Mod. Components can specify others..
// Bumped from 1.72: addons/core uses createHashMap (needs 2.02) — the declared minimum was stale
// relative to what the code actually requires; every realistic Arma 3 install is already well
// past 2.02, so this only corrects the metadata, it doesn't raise a real-world requirement.
#define REQUIRED_VERSION 2.02
#define REQUIRED_CBA_VERSION {3,0,0}
