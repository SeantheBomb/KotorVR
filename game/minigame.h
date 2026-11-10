#pragma once

// KotOR's minigames (the area's .are MiniGame struct): the swoop races (Taris m03mg, Tatooine m17mg, Manaan m26mg)
// and the Ebon Hawk's turrets (m12ab).
enum MinigameType { MG_NONE = 0, MG_SWOOP = 1, MG_TURRET = 2 };

// The minigame of the area the player is in (cached; re-read at most every 250 ms).
int Minigame_Type();
