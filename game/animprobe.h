// [Debug] AnimProbe=1: read-only walk of the player's client animation nodes, checked against the GL bones
// (see animprobe.cpp).
#pragma once

void AnimProbe_OnSwap();  // from the SwapBuffers hook, before VR_OnSwap
