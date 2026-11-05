// Drives the engine's own scene camera from the headset (render + culling), see enginecam.cpp.
#pragma once

// The headset eye in KotOR world coordinates this frame (false before the first applied camera).
bool EngineCam_EyeWorld(float out[3]);
// KotOR world -> tracking space (the frame of WORLD-anchored text panels), from the gameplay camera base.
bool EngineCam_WorldToTracking(const float w[3], float out[3]);

bool EngineCam_Install();
// true while the current frame's world camera was replaced at the engine level
bool EngineCam_Applied();
// verification feed: the engine's view rotation (column-major glMultMatrixf) and camera position (-glTranslatef)
void EngineCam_OnView(const float* m, const float p[3]);
// The view's heading is ours, not the game camera's (KotOR swings its camera around in combat): turned only by the
// player (radians, + = left), synced to the game camera on the first frame and after a teleport / area change.
void EngineCam_Turn(float radians);
// Our heading minus the game camera's (radians, + = ours is further left). Movement keys and mouse picking work in
// the game camera's frame, so they rotate by this.
float EngineCam_ViewYawOffset();
// World headings (radians, atan2(forward y, forward x)): our view, and the game camera (its W key walks along it).
float EngineCam_ViewYaw();
float EngineCam_GameYaw();
