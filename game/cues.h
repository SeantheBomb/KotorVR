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
    CUE_HIT_LANDED,     // the player's blow / bolt connected (the damage just landed)
    CUE_PERFECT_LANDED, // ... and it was an on-beat (+4) attack
    CUE_FORCE,          // a Force gesture fired (off hand)
    CUE_HURT,           // the player took weapon damage (not blocked)
    CUE_COUNT
};

void Cues_Init(const char* gameDir);
void Cues_Fire(CueEvent e);
// Once per frame (game thread): delayed haptic pulses.
void Cues_Update();
// Extra additive glow (0..1) for the weapon in a hand (0 left, 1 right): flashes from events + the window pulse.
float Cues_Glow(int hand);
// The glow's color (white unless a special move tinted it).
void Cues_GlowColor(int hand, float rgb[3]);
// A special move fired (1 power, 2 critical, 3 flurry, 4 power shot, 5 rapid): its own color flare and haptic pattern
// on the weapon, so you can tell which one it was.
void Cues_Move(int move);
// Spark burst on the weapon for blocks and hits: 0..1 strength (fades over 0.45 s); rgb: hot orange for hits,
// blue-white for blocks.
float Cues_Sparks(double* age, float rgb[3]);
// Force glow on a hand (0..1, fades over 0.5 s) after a Force gesture.
float Cues_ForceGlow(int hand);
// Blade knock-off after a block / parry: degrees to rotate the drawn weapon off the hand now (0 = none); yawSign
// picks the side it is knocked to.
float Cues_KnockOff(float* yawSign);
// After a Power Attack the blade follows the hand with this much lag (seconds, 0 = none): a heavier swing.
float Cues_PowerLag();
// An action that would have walked the character to its target was cancelled (out of reach).
void Cues_TooFar();
// The blade touched an enemy (v2 contact): the feel of the cut, scaled by the swing's tip speed (m/s).
void Cues_Contact(float speed);
// ... but too slowly to count (below the swing gate): a faint spark, a light tick.
void Cues_Glance();
// A cut that did not count (too slow, outside the trunk, or into an attack): a dull grey spark, a low double buzz.
void Cues_NoCount();
// An enemy in reach just opened up: a soft pulse in the sword hand.
void Cues_Opening();
// The player's perfect attack window is open (drives the gentle weapon pulse).
void Cues_SetWindow(bool open);
// Lightsaber audio (KotOR's own power-up / power-down / hum / swing sounds): ignition edges, and every frame the
// hum's state plus the blade tip speed (m/s), which bends its pitch and volume and triggers swing whooshes.
void Cues_SaberIgnite(bool on);
// A blaster shot leaves the barrel (the trigger was pulled): the weapon's own fire sound, right away.
void Cues_Shot();
// A KotOR sound by resref (kvr\sounds), once.
void Cues_Tone(const char* resref, float gain, float ratio);
void Cues_SaberHum(bool on, float tipSpeed);
