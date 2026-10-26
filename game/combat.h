// Skill layer over the d20 attack roll (timed attacks, block / deflect / dodge). See combat.cpp.
#pragma once

bool Combat_Install();
void Combat_EndFrame();
// true when any attack roll (anyone, e.g. a kinrath on a companion) resolved within the last ms milliseconds
bool Combat_AttackWithin(unsigned long ms);
// Once per frame (game thread): incoming-attack warnings, the player's rhythm window.
void Combat_Update();
// The player's next attack roll is less than 0.6 s away: an attack gesture now is "on the beat" (+4).
bool Combat_InPerfectWindow();
// The controlled character is already attacking (rolled within the last 3.5 s): KotOR repeats the attack every
// round by itself, so gestures only count for timing; pressing attack again just fills its action queue.
bool Combat_PlayerAttacking();

// An enemy attack on the player that lands within the warning lead (or just landed).
struct CombatWarning {
    unsigned long attacker;  // server object id
    float secondsLeft;       // until the impact (negative = just landed)
};
int Combat_IncomingWarnings(CombatWarning* out, int max);
