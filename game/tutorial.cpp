// VR instruction cards (see tutorial.h). Shown through a host text panel (slot 0), remembered in kvr.ini [Tutorial].
#include "tutorial.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/kvr_shared.h"
#include "combat.h"
#include "features.h"
#include "vr.h"
#include "weapons.h"

extern void Log(const char* fmt, ...);

namespace {

char g_ini[MAX_PATH];

struct Card {
    const char* key;   // kvr.ini [Tutorial] key, 1 once shown
    const char* text;  // first line '#' = title
};

const Card kCards[] = {
    {"Welcome",
     "#Welcome to KotOR VR\n"
     "Left stick: walk where you look. Right stick: turn.\n"
     "Point and squeeze the right grip to use, open or talk to what your hand points at.\n"
     "Y shows the game's menu panel, B is back / Escape, A pauses."},
    {"Combat",
     "#Fighting\n"
     "Swing to attack. Swing when your weapon pulses for a bonus.\n"
     "A chevron over an enemy turns gold just before it strikes: raise your blade across your body to parry.\n"
     "Overhead chop: Power Attack. Thrust: Critical Strike. Two quick swings: Flurry."},
    {"Blaster",
     "#Blasters\n"
     "Point at a target and pull the trigger: you shoot what you aim at.\n"
     "Hold your aim steady for a moment for a Power Blast; two quick shots for Rapid Shot."},
};
const int kCount = sizeof kCards / sizeof kCards[0];

int g_showing = -1;
double g_shownAt;
float g_shownPos[3];

double Now() { return GetTickCount() / 1000.0; }

bool Shown(int i) { return GetPrivateProfileIntA("Tutorial", kCards[i].key, 0, g_ini) != 0; }

void Show(int i) {
    if (g_showing >= 0 || Shown(i)) return;
    // world-locked: 1.4 m ahead of where you look, a little below eye level, facing you
    float head[3];
    VR_HeadPos(head);
    float yaw = VR_HeadYaw();
    float fx = -sinf(yaw), fz = -cosf(yaw);
    float pos[3] = {head[0] + fx * 1.4f, head[1] - 0.25f, head[2] + fz * 1.4f};
    float q[4] = {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};  // about +Y: the quad's +Z faces back at you
    VR_ShowText(0, KVR_ANCHOR_WORLD, pos, q, 1.1f, kCards[i].text, -1);
    WritePrivateProfileStringA("Tutorial", kCards[i].key, "1", g_ini);
    g_showing = i;
    g_shownAt = Now();
    memcpy(g_shownPos, head, sizeof g_shownPos);
    Log("tutorial: %s", kCards[i].key);
}

}  // namespace

void Tutorial_Init(const char* gameDir) { sprintf_s(g_ini, "%s\\kvr\\kvr.ini", gameDir); }

void Tutorial_Update() {
    if (!VR_IsActive()) return;
    if (g_showing >= 0) {
        float head[3];
        VR_HeadPos(head);
        float dx = head[0] - g_shownPos[0], dz = head[2] - g_shownPos[2];
        if (Now() - g_shownAt > 12.0 || dx * dx + dz * dz > 2.5f * 2.5f) {  // read, or walked on
            VR_HideText(0);
            g_showing = -1;
        }
        return;
    }
    static double worldSince = -1;
    if (Weapon_PlayerCreature()) {
        if (worldSince < 0) worldSince = Now();
        if (Now() - worldSince > 3.0) Show(0);
    } else {
        worldSince = -1;
    }
    if (Weapon_PlayerInCombat()) Show(1);
    WeaponClass r = Weapon_Right();
    if (r == WC_PISTOL || r == WC_RIFLE || r == WC_REPEATER) Show(2);
}
