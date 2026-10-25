// Real weapon meshes in the player's hands, from files exported by tools/weapon_export.py into <game>\kvr\.
#pragma once

void WeaponModels_Init(const char* gameDir);
// Draw the model for base item `row` / model `variation` in the current modelview (grip frame: tip along -Z).
// Returns false when no exported model exists for it (the caller then draws the proxy). `glow` (0..1) adds an
// additive white sheen over the whole weapon (combat cue flashes / timing pulse).
bool WeaponModels_Draw(int row, int variation, float glow = 0);
