// Feature switches read from <game>\kvr\kvr.ini (section [Features], 1 = on, 0 = off; all on by default).
// Lets a feature be turned off for a play session without a rebuild.
#pragma once

struct FeatureFlags {
    bool engineCamera;   // drive the engine camera (render + culling) from the headset
    bool weaponProxies;  // draw the equipped weapons in the hands
    bool gestures;       // melee swings / shots / Force gestures trigger actions
    bool combat;         // timing bonus and block / dodge on the d20 roll
    bool weaponGallery;  // [Debug] WeaponGallery=1: cycle the in-hand model through every class (screenshots)
    bool tracePosition;  // [Debug] TracePosition=1: log the player camera position and heading every 2 s (harness navigation)
};

const FeatureFlags& Features();
void Features_Load(const char* gameDir);
