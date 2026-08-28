/*
  TEMPORARILY DISABLED — see addon/README.md "Known HEMTT build findings".

  Original content was just:
    #include "\a3\ui_f_curator\UI\Displays\RscDisplayAttributes.sqf"
  which needs a P-Drive/Arma 3 install to resolve at HEMTT build time; automatic P-Drive/on-demand
  detection didn't pick up this machine's Arma 3 install for reasons not yet root-caused. Note that
  as written, that include only pulled in a *config*-shaped file (BI's RscDisplayAttributes class
  body) as the entire body of a callable SQF function (called from CfgVehicles.hpp's Zeus module
  onLoad/onUnload as ["onLoad"|"onUnload", _this, "RscDisplayAttributesModuleTFARStaticRadio"]) —
  that isn't valid runtime SQF, so this was very likely already non-functional before this change,
  not something this pass broke. Kept as a proper no-op respecting the calling convention so the
  Zeus module's event handlers still have a valid function to call.
*/
params ["", "", ""];
