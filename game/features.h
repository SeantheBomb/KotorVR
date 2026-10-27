// Feature switches read from <game>\kvr\kvr.ini (section [Features], 1 = on, 0 = off; all on by default).
// Lets a feature be turned off for a play session without a rebuild.
#pragma once

struct FeatureFlags {
    bool engineCamera;   // drive the engine camera (render + culling) from the headset
    bool weaponProxies;  // draw the equipped weapons in the hands
    bool gestures;       // melee swings / shots / Force gestures trigger actions
    bool combat;         // timing bonus and block / dodge on the d20 roll
    bool weaponGallery;  // [Debug] WeaponGallery=1: cycle the in-hand model through every class (screenshots)
    bool hands;          // draw the character's own hands (from their body model, so they match the outfit)
    bool modelCull;      // hide exactly the controlled character's model (0 = old radius heuristics)
    bool cues;           // combat feedback: haptics, weapon glow, KotOR sounds for blocks / timing / warnings
    bool cueTest;        // [Debug] CueTest=1: fire every combat cue in turn every 1.5 s (harness checks)
    bool snapTurn;       // [Comfort] SnapTurn=1: right stick turns in SnapAngle steps instead of smoothly
    float snapAngle;     // [Comfort] SnapAngle (degrees, default 30)
    float turnSpeed;     // [Comfort] TurnSpeed (degrees per second at full stick, default 120)
    bool cutsceneFirstPerson;  // [Comfort] CutsceneFirstPerson=1: never cut away from first person in cutscenes
    bool traceWorld;     // [Debug] TraceWorld=1: one GL trace of a world frame (kvr\trace_f*.txt)
    bool animProbe;      // [Debug] AnimProbe=1: log the player model's animation nodes vs the GL bones every 5 s
    bool tracePosition;  // [Debug] TracePosition=1: log the player camera position and heading every 2 s (harness navigation)
};

const FeatureFlags& Features();
void Features_Load(const char* gameDir);
// <game>\kvr\kvr.ini (for sections read live, e.g. [WeaponPose])
const char* Features_IniPath();
