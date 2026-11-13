// Show-don't-tell hints for the basics and blasters (see hints.cpp): demonstrations at the place they matter.
#pragma once

struct KvrInput;

void Hints_Init(const char* gameDir);
// Once per frame (game thread) with the latest controller state.
void Hints_Update(const KvrInput& in);
// A hint is on show (the warnings pass must draw).
bool Hints_Drawing();
// Drawn with the warnings (weapons.cpp).
void Hints_Draw(const float worldView[16]);
