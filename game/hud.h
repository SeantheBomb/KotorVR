// The flat HUD's layout (screen rectangles of its menu row, minimap, party, quick slots, toggles), read from the
// game's main interface and published in shared memory for the host's wrist panel (docs/DIEGETIC_HUD.md).
#pragma once

// Once per frame (game thread, in the world).
void Hud_Update();
