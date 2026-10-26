// Real weapon meshes in the player's hands, from files exported by tools/weapon_export.py into <game>\kvr\.
#pragma once
#include <string>

void WeaponModels_Init(const char* gameDir);
// Draw the model for base item `row` / model `variation` in the current modelview (grip frame: tip along -Z).
// Returns false when no exported model exists for it (the caller then draws the proxy). `glow` (0..1) adds an
// additive white sheen over the whole weapon (combat cue flashes / timing pulse). `blade` (0..1) is how far the
// saber blades are extended (0 = hilt only, ignition animates it).
bool WeaponModels_Draw(int row, int variation, float glow = 0, float blade = 1);
// Hands from the character's own body model (exported by tools/hand_export.py to <game>\kvr\hands), drawn in the
// current modelview = the hand bone's frame (fingers along -Z). side 0 left / 1 right; curl 0 open .. 1 fist for the
// middle / ring / little fingers and thumb (grip), index separately (trigger). texture "" = the model's own.
// Returns false when that body model has no exported hand.
bool WeaponModels_DrawHand(const char* model, const char* texture, int side, float curl, float indexCurl);
// Body model + texture for appearance.2da row `appearance`, body letter 0..9 (a..j) and armor TextureVar (from
// kvr\handsppearance.txt). False if the row has no body model for that letter.
bool WeaponModels_BodyFor(int appearance, int letter, int texVar, std::string* model, std::string* texture);
