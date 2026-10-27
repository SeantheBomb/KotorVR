// v2 contact combat core: the blade in the world, sides, contacts and hit credits. See melee.cpp.
#pragma once

#include <windows.h>

// The drawn weapon's grip and tip in KotOR world coordinates this frame (weapon pass), per hand (1 right, 0 left).
void Melee_SetBlade(int hand, const float base[3], const float tip[3]);
void Melee_ClearBlade(int hand);
// Every attack roll (combat.cpp): who fights whom.
void Melee_NoteAttack(DWORD attacker, DWORD target);
bool Melee_IsEnemy(DWORD id);
// Once per frame (game thread): blade contacts.
void Melee_Update();
// A contact / aimed shot credited this enemy in the last 1.6 s: the player's blow / bolt landing on it may count
// (Take consumes it; speed = the swing's tip speed, 1 for shots; head = a head shot).
bool Melee_TakeCredit(DWORD target, float* speed, bool* head);
bool Melee_HasCredit(DWORD target);
// An enemy your blade touched while you weren't attacking: start attacking it (0 = none).
DWORD Melee_TakeAttackStart();
// Defence: is a blade within radius of a point / of a line segment (speed: the blade's recent peak tip speed).
bool Melee_BladeNear(const float point[3], float radius, float* speed);
bool Melee_BladeCrossesLine(const float a[3], const float b[3], float radius, float* speed);
// A trigger shot along the barrel (world): credits the first enemy body it passes through.
void Melee_Shot(const float origin[3], const float dir[3]);
// Force aim: the left controller's ray (world) each frame, and the enemy it points at (within 25 deg, 15 m; 0 none).
void Melee_SetPalm(const float origin[3], const float dir[3]);
DWORD Melee_PalmTarget();
