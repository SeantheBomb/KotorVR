// Target actions in the world: KotOR's target action menu (attack moves, Force powers, security / bash, grenades, ...)
// as a panel along your hand that you point at and pick with the trigger (see actionring.cpp).
#pragma once
#include <windows.h>

struct KvrInput;

void ActionRing_Init(const char* gameDir);
// Opens the panel for the current target; false (nothing shown) when the target has no extra actions.
bool ActionRing_Open(const KvrInput& in);
// The panel is up (the trigger belongs to it).
bool ActionRing_Active();
// Once per frame (game thread). Returns a DirectInput key to tap (the picked action's column key) or 0.
BYTE ActionRing_Update(const KvrInput& in);
// Clear the controlled character's action queue - what the HUD's "clear all actions" button does (0x6887D0 on the
// in-game interface). False if the interface is not up.
bool ActionRing_ClearQueue();
