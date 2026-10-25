// Combat feedback cues (Arkham-style readability on top of KotOR's d20): controller haptics through the host, a glow
// on the weapon in hand, and KotOR's own combat / GUI sounds (exported to <game>\kvr\sounds by tools\sound_export.py).
#pragma once

enum CueEvent {
    CUE_BLOCK,          // incoming attack turned into a miss by the guard
    CUE_PERFECT_BLOCK,  // ... with the guard raised in the perfect window around the predicted attack
    CUE_DODGE,          // incoming attack turned into a miss by a dodge
    CUE_BLOCK_FAILED,   // guard / dodge was up but the attack still hit
    CUE_PERFECT_HIT,    // attack gesture inside the player's perfect timing window
    CUE_WINDOW_OPEN,    // the player's perfect attack window just opened
    CUE_INCOMING,       // an enemy attack on the player is about to resolve (warning)
    CUE_COUNT
};

void Cues_Init(const char* gameDir);
void Cues_Fire(CueEvent e);
// Once per frame (game thread): delayed haptic pulses.
void Cues_Update();
// Extra additive glow (0..1) for the weapon in a hand (0 left, 1 right): flashes from events + the window pulse.
float Cues_Glow(int hand);
// The player's perfect attack window is open (drives the gentle weapon pulse).
void Cues_SetWindow(bool open);
