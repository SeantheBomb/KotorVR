// Debug commands from the test harness (kvr_simhost "game ..." lines in <game>\kvr\simcmd.txt), see simcmd.cpp.
#pragma once

#include <windows.h>

void SimCmd_Init(const char* gameDir);
// Once per frame from Input_Update, after the stick's movement keys: runs pending commands and, while the autopilot
// drives, replaces the movement keys (W/S/Z/C) in `keys` and turns the view.
void SimCmd_Update(BYTE keys[256]);
// The autopilot is steering (the stick's own movement rules stay out of its way).
bool SimCmd_Driving();
