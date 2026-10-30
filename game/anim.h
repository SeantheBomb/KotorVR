// Client animation hooks (v2): per-creature hit-pause through Gob::Animate (see anim.cpp).
#pragma once

bool Anim_Install();
// Once per frame (game thread): keeps the hidden body's combat event filter on the controlled character's model.
void Anim_Update();
// The controlled character's body model's world position (exactly where its hierarchy is drawn).
bool Anim_PlayerBodyPos(float out[3]);
// How high the controlled character's head is above its feet (m; -99 unknown): ~1.6 standing, much less lying down.
float Anim_PlayerHeadHeight();
// A hit at a world point on this creature: its upper body flinches away from it for a quarter second.
void Anim_Flinch(unsigned long targetId, const float hit[3]);
// v2 per-bone hit volumes: the creature model standing at this world position (nullptr if none animated there) and
// its skeleton as world-space capsules (parent -> child bone), up to max.
struct AnimBone {
    float a[3], b[3];
    float radius;
    bool head;
};
void* Anim_ModelAt(const float pos[3]);
int Anim_BoneSegments(void* model, AnimBone* out, int max);
// A named node of that model (e.g. "rhand", the weapon hook), world position.
bool Anim_PartPos(void* model, const char* name, float out[3]);
// Freeze the animation of the creature with this server object id for `seconds` (a hit landing on it).
void Anim_HitPause(unsigned long targetId, float seconds);
