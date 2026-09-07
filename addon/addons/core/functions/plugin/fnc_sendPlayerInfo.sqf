#include "script_component.hpp"

/*
  Name: TFAR_fnc_sendPlayerInfo

  Author: NKey
    Notifies the plugin about a player

  Arguments:
    0: Unit <OBJECT>
    1: Is unit close to player <BOOL>
    2: Unit name <STRING>

  Return Value:
    None

  Example:
    [player, true, "Guy"] call TFAR_fnc_sendPlayerInfo;

  Public: Yes
*/

params ["_player"];

private _request = _this call TFAR_fnc_preparePositionCoordinates;
private _result = "task_force_radio_pipe" callExtension _request;

_splitResult = _result splitString "";

if (_result != "OK") then {

    // The extension's wording is legacy and deliberately unchanged on the wire (community addons
    // may still match the exact string -- see docs/protocol-extension-legacy.md), but showing it
    // to the player verbatim is actively misleading: this build has no TeamSpeak dependency at
    // all, and people were installing and starting TS3 to try to make the message go away.
    if (_result == "Not connected to TeamSpeak") exitWith {
        if !(GVAR(noTSNotConnectedHint)) then {
            [parseText localize LSTRING(WM_NotConnectedToVoiceServer), 10] call TFAR_fnc_showHint;
            tf_lastError = true;
        };
    };

    [parseText (_result), 10] call TFAR_fnc_showHint;
    tf_lastError = true;
} else {
    if (tf_lastError) then {
        call TFAR_fnc_hideHint;
        tf_lastError = false;
    };
};

//#TODO this is a bad place to do it, why check every update...
if !(_player getVariable ["TFAR_killedEHAttached",false]) then {
    _player addEventHandler ["Killed", {_player call TFAR_fnc_sendPlayerKilled}];
    _player setVariable ["TFAR_killedEHAttached", true];
};

// Tell the extension this unit's real getPlayerUID (see "UID" in
// docs/protocol-extension-legacy.md). Lets nickname<->relay-UID resolution come straight from
// Arma instead of matching display names, which aren't guaranteed unique.
//
// Tracked with its OWN flag, and retried until Arma actually hands us a UID. This used to live
// inside the Killed-EH branch above, which meant one attempt per unit, ever: whenever
// getPlayerUID happened to return "" at that exact moment -- a JIP player whose unit we already
// saw, a slot still AI-controlled, a remote-controlled/curator unit -- the flag was set anyway
// and no UID was ever sent for that name again. The extension then had no way to tie that
// player's voice session to their unit, which is precisely "everyone hears him except me" /
// "nobody could hear me until I rejoined". For our OWN unit it was worse still: without a UID the
// extension has no identity to hand the voice server, which rejects an empty one outright, so the
// client could never connect at all and just kept reporting "not connected".
if !(_player getVariable ["TFAR_uidSent", false]) then {
    private _steamUid = getPlayerUID _player;
    if (_steamUid != "") then {
        _player setVariable ["TFAR_uidSent", true];
        private _uidMsg = format ["UID	%1	%2~", _this select 2, _steamUid];
        "task_force_radio_pipe" callExtension _uidMsg;
    };
};
