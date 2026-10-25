// Skill layer over the d20 attack roll (timed attacks, block / deflect / dodge). See combat.cpp.
#pragma once

bool Combat_Install();
void Combat_EndFrame();
// true when any attack roll (anyone, e.g. a kinrath on a companion) resolved within the last ms milliseconds
bool Combat_AttackWithin(unsigned long ms);
