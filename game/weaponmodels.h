// Real weapon meshes in the player's hands, from files exported by tools/weapon_export.py into <game>\kvr\.
#pragma once

void WeaponModels_Init(const char* gameDir);
// Draw the model for base item `row` / model `variation` in the current modelview (grip frame: tip along -Z).
// Returns false when no exported model exists for it (the caller then draws the proxy).
bool WeaponModels_Draw(int row, int variation);
