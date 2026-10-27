// The controlled character's own models (body, head, held weapons) recognised by the size of each mesh draw
// (index count), from kvr\models\signatures.txt (tools/sig_export.py). Lets the model cull hide exactly the
// character's own pieces near the camera - a weapon swung far out in an attack included - without hiding scenery
// that happens to stand close (crates, consoles, door frames).
#pragma once

void OwnSig_Init(const char* gameDir);
// The current character: appearance.2da row (for the head), body model, held weapon models (resrefs).
void OwnSig_SetCharacter(int appearance, const char* bodyModel, const char* weapons[], int nWeapons);
// A draw of this many indices belongs to one of the character's own meshes.
bool OwnSig_Match(int indexCount);
bool OwnSig_Ready();
