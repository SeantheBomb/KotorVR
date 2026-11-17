// Debilitated states (Stun, Paralysis, Sleep, Horror, Choke) in VR (2026-11-17 playtest: the only sign of a stun was
// the small "Player debilitated" line in the corner).
//
// KotOR plays a looping animation on a debilitated creature (animations.2da: paralyzed, sleep, horror, choke); the
// controlled character's hidden body starting one of them is the signal, and any other animation it starts ends it.
// While it lasts: a slow violet pulse round the edge of the view, a rumble on both hands as it starts, and (unless
// [Comfort] StunCamera=0) the camera eases out of your body to show it reeling, as the death camera does.
#include "stun.h"

#include <windows.h>

#include <cstring>

#include "features.h"
#include "vr.h"

extern void Log(const char* fmt, ...);

namespace {

bool g_active;
double g_since, g_lastPulse;
char g_anim[32];

double Now() { return GetTickCount() / 1000.0; }

bool Debilitating(const char* n) {
    static const char* kLoops[] = {"paralyzed", "sleep", "horror", "choke"};
    for (const char* k : kLoops)
        if (!strcmp(n, k)) return true;
    return strstr(n, "stun") != nullptr || strstr(n, "daze") != nullptr;
}

}  // namespace

void Stun_OnPlayerAnim(const char* name) {
    bool deb = Debilitating(name);
    if (deb && !g_active) {
        g_active = true;
        g_since = Now();
        g_lastPulse = -10;
        strncpy_s(g_anim, name, _TRUNCATE);
        VR_Haptic(0, 0.8f, 0.35f, 40);
        VR_Haptic(1, 0.8f, 0.35f, 40);
        Log("stun: you are debilitated (%s)%s", name, Stun_Camera() ? " - camera steps out of your body" : "");
    } else if (!deb && g_active) {
        g_active = false;
        Log("stun: over after %.1f s (%s -> %s)", Now() - g_since, g_anim, name);
    }
}

void Stun_Update() {
    if (!g_active) return;
    double t = Now();
    if (t - g_since > 30.0) {  // never stuck on: no stun lasts this long without another animation
        g_active = false;
        Log("stun: no end seen after 30 s - cleared");
        return;
    }
    if (t - g_lastPulse > 0.9) {
        g_lastPulse = t;
        VR_Flash(0.55f, 0.35f, 1.0f, 0.55f, 0.85f);
    }
}

bool Stun_Active() { return g_active; }

bool Stun_Camera() {
    static int on = -1;
    if (on < 0) on = GetPrivateProfileIntA("Comfort", "StunCamera", 1, Features_IniPath());
    return on != 0;
}
