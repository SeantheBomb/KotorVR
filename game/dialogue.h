// Conversations in the world: KotOR's reply list as a panel in front of you that you point at and pick with the
// trigger (see dialogue.cpp).
#pragma once
#include <windows.h>

struct KvrInput;

bool Dialogue_Install();
// Once per frame (game thread). Returns a DirectInput key to tap (the reply's number key) or 0.
BYTE Dialogue_Update(const KvrInput& in);
// The right hand points at a reply on the conversation panel (the trigger belongs to it).
bool Dialogue_Active();
// A / B pressed during a line with no replies: hold the left mouse button down this frame (the panel's skip).
bool Dialogue_SkipClick();
// Our conversation panel is up (any line, with or without replies).
bool Dialogue_Showing();
// Where the conversation's owner (the one you talk with) stands, world feet position.
bool Dialogue_SpeakerPos(float out[3]);
// Seconds since the last conversation ended (huge if none yet).
double Dialogue_EndedAgo();
