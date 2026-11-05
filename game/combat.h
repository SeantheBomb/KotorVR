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
// v2: your blade / barrel decides what lands ([Combat] Mode=contact, the default).
bool Combat_ContactMode();
// The player's queued attack was cleared: the next swing starts a new one.
void Combat_ResetAttacking();
// End the player's running combat round now (its timer run out): the stick gets the character back at once.
void Combat_EndPlayerRound();

// An enemy attack on the player, from its roll until just after it lands (the telegraph).
struct CombatWarning {
    unsigned long attacker;  // server object id
    float secondsLeft;       // until the impact (negative = just landed)
    float total;             // roll -> impact, seconds (for the closing timing ring)
    bool ranged;             // a bolt (dodge / deflect) rather than a blow (block / parry)
};
int Combat_IncomingWarnings(CombatWarning* out, int max);
// The warning lead: incoming-attack cue / gold parry chevron this long before the impact.
const float kCombatWarnLead = 0.6f;

// A 5+ byte detour at va whose first len bytes must equal orig (absolute operands only): stub runs first, then
// jumps to *tramp (the original). Shared by the combat and animation hooks.
bool Combat_Detour(unsigned long va, const unsigned char* orig, int len, void* stub, unsigned char** tramp,
                   const char* what);
