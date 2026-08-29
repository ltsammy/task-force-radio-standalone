#include "script_component.hpp"

/*
  Name: TFAR_fnc_updateMuteIndicatorUI

  Author: Claude
    Shows/hides the mic-mute and speaker-mute HUD hint based on the extension's current state.
    Polled periodically (see fnc_ClientInit.sqf) rather than event-driven, since mute state can
    also change from outside the mission (e.g. reconnecting), not just the MicMute/SpeakerMute
    keybinds.

  Arguments:
    None

  Return Value:
    None

  Example:
    call TFAR_fnc_updateMuteIndicatorUI;

  Public: Yes
*/

private _display = uiNamespace getVariable [QGVAR(HUDVolumeIndicatorRscDisplay), displayNull];
if (isNull _display) exitWith {};

private _state = "task_force_radio_pipe" callExtension "MUTE_STATE";

(_display displayCtrl 1201) ctrlShow ((_state select [0, 1]) == "1");
(_display displayCtrl 1202) ctrlShow ((_state select [1, 1]) == "1");
