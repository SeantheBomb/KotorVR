// The name of what the right hand points at in the world, as a small label at the cursor (see hover.cpp).
#pragma once

#include "../common/kvr_shared.h"

// Once per frame (game thread).
void Hover_Update(const KvrInput& in);
// A grip / shot acted on the hovered object: the label flashes.
void Hover_Interacted();
// Something else uses the label slot for a moment (block / parry results): hover stays hidden that long.
void Hover_Suppress(double seconds);
