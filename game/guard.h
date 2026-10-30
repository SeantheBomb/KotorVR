// Melee defence target: the block bar for each incoming blow (see guard.cpp).
#pragma once

#include <windows.h>

struct GuardBar {
    DWORD attacker;
    float a[3], b[3];      // the bar's ends (world)
    float hand[3];         // the attacker's weapon hand (world), for the line to the bar
    bool haveHand;
    bool frozen;           // the last moments: the bar holds still
    bool lined;            // your blade is on it
    float secondsLeft, total;
    int result;            // 0 pending, 1 blocked, 2 parried, 3 missed
    float resultAge;       // seconds since the result
};

// Once per frame (game thread): bars for the incoming blows, the lined-up feedback.
void Guard_Update();
int Guard_Bars(GuardBar* out, int max);
// At the impact (combat.cpp): is the blade on this attacker's bar (true: *parry says if it was a parry)?
// false with *haveBar false when there is no bar for it (no fresh data): the caller falls back.
bool Guard_Judge(DWORD attacker, bool* parry, float* speed, bool* haveBar);
// The outcome, for the feedback (1 blocked, 2 parried, 3 missed).
void Guard_Result(DWORD attacker, int result);
