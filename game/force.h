// Force powers by gesture: gather with the left grip, the release picks the family, the hold the tier. See force.cpp.
#pragma once

#include <windows.h>

#include "../common/kvr_shared.h"

// Once per frame (game thread). Returns a key to tap (a target column, DIK_1..3) when a power is being cast.
BYTE Force_Update(const KvrInput& in, bool fighting);
// A parry: a little Force flows back.
void Force_OnParry();
// The saber is in the air (Saber Throw) until you catch it.
bool Force_SaberThrown();
// A grenade is in your left hand (armed at the hip, not yet thrown).
bool Force_GrenadeArmed();
