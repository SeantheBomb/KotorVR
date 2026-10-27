// Client animation hooks (v2): per-creature hit-pause through Gob::Animate (see anim.cpp).
#pragma once

bool Anim_Install();
// Freeze the animation of the creature with this server object id for `seconds` (a hit landing on it).
void Anim_HitPause(unsigned long targetId, float seconds);
