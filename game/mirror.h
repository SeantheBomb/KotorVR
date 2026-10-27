// Desktop mirror: the game's own window shows the headset's left eye with the game's GUI on top (mirror.cpp).
#pragma once
#include <windows.h>

// Right after a left-eye image is finished (before the GUI pass clears it): keep it. Current GL context.
void Mirror_KeepEye(int w, int h, PROC(WINAPI* getProc)(LPCSTR));
// At swap, on world frames whose back buffer holds only the GUI: put the kept eye under it.
void Mirror_Draw();
