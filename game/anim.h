// Client animation hooks (v2): per-creature hit-pause through Gob::Animate (see anim.cpp).
#pragma once

bool Anim_Install();
// Once per frame (game thread): keeps the hidden body's combat event filter on the controlled character's model.
void Anim_Update();
// The controlled character's body model's world position (exactly where its hierarchy is drawn).
bool Anim_PlayerBodyPos(float out[3]);
// Freeze the animation of the creature with this server object id for `seconds` (a hit landing on it).
void Anim_HitPause(unsigned long targetId, float seconds);
