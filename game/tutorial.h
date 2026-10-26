// VR instructions in place of KotOR's keyboard tutorial popups (swkotor.ini Tutorial Popups=0): short cards shown once
// each, the first time it matters, world-locked in front of you and dismissed by time or by walking away.
#pragma once

void Tutorial_Init(const char* gameDir);
// Once per frame (game thread).
void Tutorial_Update();
