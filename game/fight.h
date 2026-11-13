// Choreographed combat ([Combat] Mode=choreo, the default): KotOR's own paired animations become the note chart.
// See fight.cpp and the design canvas (docs/COMBAT_DESIGN.md).
#pragma once

#include <windows.h>

void Fight_Init(const char* gameDir);
bool Fight_Active();  // Mode=choreo
// Once per frame (game thread), after Guard_Update.
void Fight_Update();
// Drawn with the warnings (weapons.cpp): gates, charge glows, status bands, stance ghosts, vignette.
void Fight_Draw(const float worldView[16]);

// combat.cpp: an attack is about to be rolled (resolver pre-hook). record = the attack record it will write.
void Fight_NoteRoll(DWORD attacker, DWORD target, BYTE* record, bool playerAttacks, bool targetIsPlayer);
// combat.cpp: the player's blow lands (its damage is queued now): grade it against its gate. Scales / cancels
// the 15 damage ints in place. Returns false when Fight has nothing to say (fallback to the contact rules).
bool Fight_PlayerBlowLands(DWORD target, int* ints);
// combat.cpp: an enemy blow on the player was judged at its impact (1 blocked, 2 parried, 3 missed / hurt).
void Fight_DefenceResult(DWORD attacker, int result, int damage);
// guard.cpp: the enemy's blade at its impact (world), when the choreography table knows the attack. false: none.
bool Fight_EnemyGate(DWORD attacker, float a[3], float b[3], float* secondsLeft, float* total);
// The bot owns the blade ([Debug] Bot / simcmd "bot N": 0 off, 1 sloppy, 2 perfect, 3 idle).
void Fight_SetBot(int mode);
int Fight_Bot();
// The posture-broken enemy whose opening is live (0 none).
DWORD Fight_OpenEnemy(float pos[3]);
// Fight telemetry line now (simcmd "fightsummary").
void Fight_Summary(const char* why);
// Difficulty: incoming weapon damage multiplier ([Combat] IncomingDamage).
float Fight_IncomingDamage();
// A Force power was released now: on the beat? (bonus applied by the caller)
bool Fight_OnBeat();
// A key for input.cpp to tap once (a stance's feat: its action-menu column 1..3), 0 none.
BYTE Fight_TakeKey();
// An attempt's score 0..1 from distance (m), time from the blow (s, - early) and blade speed (m/s): shared by your
// gates and the parry / block judge (guard.cpp).
float Fight_Score(float d, float dt, float speed);
// combat.cpp: a posture break opened this enemy (one outcome: the opening).
void Fight_Opening(DWORD id);
