// The Ebon Hawk's turret minigame in VR (see turret.cpp): the view stays in the gunner's seat, the right hand aims.
#pragma once
#include <windows.h>

struct KvrInput;

// Channels the aim is sent to the game on (input.cpp delivers it).
enum TurretChannel { TURRET_DI_MOUSE = 1, TURRET_CURSOR = 2, TURRET_STICK = 3 };

// Once per frame while the turret runs and no game menu is up: the keys (fire, pause; the stick's W/A/S/D only when
// the hand can't drive the turret) and the mouse movement that turns the guns toward where the right hand points.
// Returns the channel the movement is for.
int Turret_Input(const KvrInput& in, BYTE keys[256], LONG* dx, LONG* dy);
// The aim reticles and the hand's short laser, drawn with the warnings (weapons.cpp).
bool Turret_Drawing();
void Turret_Draw(const float worldView[16]);
