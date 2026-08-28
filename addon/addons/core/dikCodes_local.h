// Local stand-in for BI's "\a3\editor_f\Data\Scripts\dikCodes.h" — that absolute engine path
// needs a full Arma 3 install/P-drive at build time, which a headless CI runner doesn't have.
// These are standard, unchanging DirectInput scancode constants (not Arma-specific content),
// limited to exactly what fnc_initKeybinds.sqf uses. If more DIK_* constants are needed later,
// add them here with the standard DirectInput scancode table.

#define DIK_T 20
#define DIK_P 25
#define DIK_CAPSLOCK 58
#define DIK_UP 200
#define DIK_PRIOR 201
#define DIK_LEFT 203
#define DIK_RIGHT 205
#define DIK_NEXT 209
