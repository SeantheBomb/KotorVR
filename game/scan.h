// Development tool: locate the game camera's yaw field by following the rendered camera heading.
#pragma once

// Called with the game camera's world->camera rotation (column-major) on gameplay frames.
void Scan_OnGameplayCamera(const float R[16]);
