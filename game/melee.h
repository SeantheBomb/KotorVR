// v2 contact combat core: the blade in the world, sides, contacts and hit credits. See melee.cpp.
#pragma once

#include <windows.h>

// The drawn weapon's grip and tip in KotOR world coordinates this frame (weapon pass), per hand (1 right, 0 left).
void Melee_SetBlade(int hand, const float base[3], const float tip[3]);
void Melee_ClearBlade(int hand);
// The blade tip's speed now (m/s), 0 without a blade.
float Melee_BladeSpeed(int hand);
// Every attack roll (combat.cpp): who fights whom.
void Melee_NoteAttack(DWORD attacker, DWORD target);
bool Melee_IsEnemy(DWORD id);
// The nearest live enemy (Melee_IsEnemy) to a point: its id and position; 0 when there is none.
DWORD Melee_NearestEnemy(const float from[3], float out[3]);
// Once per frame (game thread): blade contacts.
void Melee_Update();
// A contact / aimed shot credited this enemy in the last 1.6 s: the player's blow / bolt landing on it may count
// (Take consumes it; speed = the swing's tip speed, 1 for shots; head = a head shot).
bool Melee_TakeCredit(DWORD target, float* speed, bool* head);
bool Melee_HasCredit(DWORD target);
float Melee_CreditAge(DWORD target);  // seconds since the target's credit was given, -1 for none
// An enemy your blade touched while you weren't attacking: start attacking it (0 = none).
DWORD Melee_TakeAttackStart();
// Defence: is a blade within radius of a point / of a line segment (speed: the blade's recent peak tip speed).
bool Melee_BladeNear(const float point[3], float radius, float* speed);
// ... lying along a bar a..b: within tolerance of it and at its angle (|cos| >= minCos).
bool Melee_BladeOnBar(const float a[3], const float b[3], float tolerance, float minCos, float* speed);
bool Melee_BladeCrossesLine(const float a[3], const float b[3], float radius, float* speed);
// A trigger shot along the barrel (world): credits the first enemy body it passes through.
float Melee_Shot(const float origin[3], const float dir[3]);  // distance to the enemy hit, -1 for none
// The live enemy the last trigger shot hit (once; 0 none): it becomes your target.
DWORD Melee_TakeShotTarget();
// The game's own line-of-sight test between two world points (ignoring two objects); true = clear.
bool Melee_LineOfSight(const float from[3], const float to[3], DWORD ignoreA, DWORD ignoreB);
// Force aim: the left controller's ray (world) each frame, and the enemy it points at (35 deg cone, then forgiving fallbacks).
void Melee_SetPalm(const float origin[3], const float dir[3]);
DWORD Melee_PalmTarget();
// A grenade thrown along the palm's heading lands this far out: the enemy nearest that spot within radius (0 none).
DWORD Melee_LandingTarget(float range, float radius);
