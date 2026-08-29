#include "script_component.hpp"

/*
  Name: TFAR_fnc_onMicPTTReleased

  Author: Claude
    Fired when the keybinding for direct-speech Mic PTT is released.

  Arguments:
    None

  Return Value:
    Whether or not the event was handled <BOOL>

  Example:
    call TFAR_fnc_onMicPTTReleased;

  Public: No
*/

"task_force_radio_pipe" callExtension "MICPTT	RELEASED~";
false
