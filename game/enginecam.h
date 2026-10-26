// Drives the engine's own scene camera from the headset (render + culling), see enginecam.cpp.
#pragma once

bool EngineCam_Install();
// true while the current frame's world camera was replaced at the engine level
bool EngineCam_Applied();
// verification feed: the engine's view rotation (column-major glMultMatrixf) and camera position (-glTranslatef)
void EngineCam_OnView(const float* m, const float p[3]);
