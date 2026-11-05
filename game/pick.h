// What the right hand's ray actually points at in the world (see pick.cpp).
#pragma once

#include <windows.h>

// Loads kvr/models/doors.txt (tools/door_export.py): door boxes for the ray.
void Pick_Init(const char* gameDir);
// The right hand's aim ray in world coordinates (weapons.cpp, each rendered eye).
void Pick_SetAim(const float origin[3], const float dir[3]);
// Once per frame (game thread): test the ray against the area's doors, placeables, items and creatures.
void Pick_Update();
// This frame's pick: the nearest object the ray passes through (in line of sight) and the point on the ray inside it.
bool Pick_Hit(DWORD* serverId, float point[3]);
// What this frame's pick is: "door", "creature", "placeable", "item" or "nothing" (test harness).
const char* Pick_HitKind();
// Log the area's doors: how many there are, static, without a box, and how many the ray can pick (test harness).
void Pick_LogDoors();
// Whether the ray passes through / close to this object this frame (any of them, not only the nearest).
// True when there is no fresh aim (the pick has no opinion).
bool Pick_Touches(DWORD serverId);
// The aim is fresh and picks are judged.
bool Pick_Active();
