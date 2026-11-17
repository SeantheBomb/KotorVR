// Being stunned (and the other debilitated states) made obvious in VR: see stun.cpp.
#pragma once

// Every animation the controlled character's hidden body starts (fight.cpp's PlayAnimation hook), lower case.
void Stun_OnPlayerAnim(const char* name);
// Once per frame (game thread): the pulsing vignette.
void Stun_Update();
// Stunned / paralysed / asleep / horrified / choked right now.
bool Stun_Active();
// [Comfort] StunCamera=1 (default): while stunned the camera steps out of your body to show it reeling.
bool Stun_Camera();
