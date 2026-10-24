// Controller input injection through the game's own DirectInput devices.
#pragma once
#include <windows.h>

// Wrap the IDirectInput8A the game creates so its keyboard and mouse devices can be augmented.
void* Input_WrapDirectInput(void* realDI8A);
// Called once per game frame (from the swap hook) with the window that is being presented.
void Input_Update(HWND wnd);
// The game moved its own cursor (its SetCursorPos call), in screen coordinates.
void Input_OnGameSetCursorPos(int x, int y);
// The game queried the hardware cursor; may rewrite the position to the laser target.
void Input_OnGameGetCursorInfo(PCURSORINFO ci);
