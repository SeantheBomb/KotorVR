// Motion gestures from the hand/head pose stream: melee swings, guard posture, dodges, aim steadiness, Force moves.
#pragma once

struct KvrInput;

// Once per game frame with the latest controller state and the head (eye 0) position, both in the recenter-origin
// frame (GL camera space: +X right, +Y up, -Z forward, meters).
void Gestures_Update(const KvrInput& in, const float headPos[3]);

// A committed melee swing just happened (consumed by the caller). quality 1..2 (speed relative to the profile's
// threshold). Also remembered for the attack resolver as "the last swing".
bool Gestures_TakeSwing(float* quality);
// Most recent swing/shot quality within `seconds`, consumed once read (0 if none). Used by the attack resolver.
float Gestures_ConsumeRecentAttack(double seconds);
// A ranged shot was fired now (trigger pull); records aim steadiness as its quality.
void Gestures_OnShot();

// Defensive state within the last `seconds`.
bool Gestures_Guarding(double seconds);  // melee weapon raised across the body in front
bool Gestures_Dodged(double seconds);    // head moved sharply sideways or down
// Seconds the guard has been held right now (-1 when not guarding). A guard raised just before an impact = parry.
double Gestures_GuardHeldFor();
// Seconds since the last attack gesture (swing / shot), large if none.
double Gestures_LastAttackAgo();

// Special moves read from the attack gestures (mapped to KotOR's combat feats by combat.cpp):
//   melee  power = big overhead downward swing, critical = forward thrust, flurry = 2+ swings within 1 s
//   ranged power = shot after holding the aim steady 1.2 s, rapid = 2+ shots within 1 s
enum GestureMove { MOVE_NONE, MOVE_POWER, MOVE_CRITICAL, MOVE_FLURRY, MOVE_POWER_SHOT, MOVE_RAPID, MOVE_SNIPER };
// The last special move within `seconds`, consumed once read.
GestureMove Gestures_TakeMove(double seconds);

bool Gestures_TakeForcePush();
// A special move chosen by something other than a swing gesture (the blaster's fire modes).
void Gestures_QueueMove(GestureMove m);  // off hand thrust forward
bool Gestures_TakeForcePull();  // off hand yanked back

double Gestures_Now();
