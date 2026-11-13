// Show, don't tell (2026-11-13: "throwing a wall of text at me in VR accomplished very little"): the Welcome and
// Blaster cards became demonstrations. Each hint appears where and when it matters, shows the thing itself (a trail on
// the floor, an arc around you, brackets on what you point at, a line from your barrel), carries a two- or three-word
// label in the game's own font, and goes away for good once you have done it ([Tutorial] in kvr.ini; set a key to 0
// to see it again).
//
//   Walk    a trail of chevrons on the floor ahead          until you have moved 2 m          "Left stick: walk"
//   Turn    an arc on the floor around you                  until you have turned 40 deg      "Right stick: turn"
//   Use     brackets on the door / person / container       until you grip on one             "Grip to use"
//   Hud     at your left wrist                              until your palm faces you         "Palm to face: HUD"
//   Back    when a game menu first opens                    for 4 s                           "B: back"
//   Shoot   a line from your barrel to the enemy            until you have fired twice        "Pull the trigger"
//   Damage  at the white burst, the first time              for 2 s                           "That's damage"
//   Brace   from your left hand to the barrel               until you hold the barrel once    "Hold the barrel: steadier"
#include "hints.h"

#include <windows.h>
#include <gl/GL.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/kvr_shared.h"
#include "combat.h"
#include "dialogue.h"
#include "enginecam.h"
#include "hover.h"
#include "melee.h"
#include "pick.h"
#include "vr.h"
#include "weapons.h"

extern void Log(const char* fmt, ...);

namespace {

enum { H_WALK, H_TURN, H_USE, H_HUD, H_BACK, H_SHOOT, H_DAMAGE, H_BRACE, H_COUNT };
const char* kKey[H_COUNT] = {"Walk", "Turn", "Use", "Hud", "Back", "Shoot", "Damage", "Brace"};
const char* kLabel[H_COUNT] = {"Left stick: walk", "Right stick: turn", "Grip to use", "Palm to face: HUD", "B: back",
                               "Pull the trigger", "That's damage", "Hold the barrel: steadier"};
char g_ini[MAX_PATH];
bool g_done[H_COUNT];
int g_active = -1;         // the hint on show
double g_activeSince = -1;
double g_worldSince = -1;
bool g_everInWorld;
float g_startPos[3];
bool g_haveStart;
float g_startYaw;
int g_shots;
double g_lastShotRoll = -10;
double g_labelUntil = -1;
float g_usePoint[3];
bool g_haveUse;

double Now() { return GetTickCount() / 1000.0; }

void Done(int h) {
    if (g_done[h]) return;
    g_done[h] = true;
    char key[32];
    sprintf_s(key, "Hint%s", kKey[h]);
    WritePrivateProfileStringA("Tutorial", key, "1", g_ini);
    Log("hints: %s done", kKey[h]);
    if (g_active == h) {
        g_active = -1;
        VR_HideText(3);
    }
}

float WrapPi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

void Label(unsigned anchor, const float pos[3], const char* text) {
    float head[3];
    VR_HeadPos(head);
    float q[4] = {0, 0, 0, 1};
    if (anchor == KVR_ANCHOR_WORLD) {
        float yaw = atan2f(head[0] - pos[0], head[2] - pos[2]);
        q[1] = sinf(yaw * 0.5f), q[3] = cosf(yaw * 0.5f);
    }
    char buf[64];
    sprintf_s(buf, "~%s", text);
    Hover_Suppress(0.5);
    VR_ShowText(3, anchor, pos, q, 0.3f, buf, -1);
    g_labelUntil = Now() + 0.4;
}

// GL (the warnings pass's context)
typedef void(APIENTRY* glBegin_t)(GLenum);
typedef void(APIENTRY* glEnd_t)();
typedef void(APIENTRY* glVertex3f_t)(GLfloat, GLfloat, GLfloat);
typedef void(APIENTRY* glColor4f_t)(GLfloat, GLfloat, GLfloat, GLfloat);
glBegin_t gB;
glEnd_t gE;
glVertex3f_t gV;
glColor4f_t gC;
typedef void(APIENTRY* glBlendFunc_t)(GLenum, GLenum);
glBlendFunc_t gBF;
bool Gl() {
    if (gB) return true;
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) return false;
    gB = (glBegin_t)GetProcAddress(gl, "glBegin");
    gE = (glEnd_t)GetProcAddress(gl, "glEnd");
    gV = (glVertex3f_t)GetProcAddress(gl, "glVertex3f");
    gC = (glColor4f_t)GetProcAddress(gl, "glColor4f");
    gBF = (glBlendFunc_t)GetProcAddress(gl, "glBlendFunc");
    return gB && gE && gV && gC && gBF;
}
void ToEye(const float wv[16], const float w[3], float e[3]) {
    for (int r = 0; r < 3; r++) e[r] = wv[0 * 4 + r] * w[0] + wv[1 * 4 + r] * w[1] + wv[2 * 4 + r] * w[2] + wv[12 + r];
}
// KotOR's hologram blue
const float kBlue[3] = {0.35f, 0.78f, 1.0f};

void Tri(const float wv[16], const float a[3], const float b[3], const float c[3], float alpha) {
    float ea[3], eb[3], ec[3];
    ToEye(wv, a, ea);
    ToEye(wv, b, eb);
    ToEye(wv, c, ec);
    if (ea[2] > -0.05f || eb[2] > -0.05f || ec[2] > -0.05f) return;
    gC(kBlue[0] * 0.55f, kBlue[1] * 0.55f, kBlue[2] * 0.6f, alpha);  // (normal blending: readable on white floors too)
    gB(GL_TRIANGLES);
    gV(ea[0], ea[1], ea[2]);
    gV(eb[0], eb[1], eb[2]);
    gV(ec[0], ec[1], ec[2]);
    gE();
}

// a floor chevron at p pointing along f (world, z up)
void Chevron(const float wv[16], const float p[3], const float f[2], float size, float alpha) {
    float r[2] = {f[1], -f[0]};
    float tip[3] = {p[0] + f[0] * size * 0.6f, p[1] + f[1] * size * 0.6f, p[2]};
    float l0[3] = {p[0] - r[0] * size, p[1] - r[1] * size, p[2]}, l1[3] = {l0[0] + f[0] * size * 0.35f, l0[1] + f[1] * size * 0.35f, p[2]};
    float r0[3] = {p[0] + r[0] * size, p[1] + r[1] * size, p[2]}, r1[3] = {r0[0] + f[0] * size * 0.35f, r0[1] + f[1] * size * 0.35f, p[2]};
    float n[3] = {tip[0] - f[0] * size * 0.35f, tip[1] - f[1] * size * 0.35f, p[2]};
    Tri(wv, l0, tip, n, alpha);
    Tri(wv, l0, n, l1, alpha);
    Tri(wv, r0, n, tip, alpha);
    Tri(wv, r0, r1, n, alpha);
}

void Line(const float wv[16], const float a[3], const float b[3], float w, float alpha, bool dashed) {
    int seg = dashed ? 14 : 1;
    for (int i = 0; i < seg; i++) {
        if (dashed && (i & 1)) continue;
        float p0[3], p1[3], e0[3], e1[3];
        for (int k = 0; k < 3; k++) p0[k] = a[k] + (b[k] - a[k]) * i / seg, p1[k] = a[k] + (b[k] - a[k]) * (i + 1) / seg;
        ToEye(wv, p0, e0);
        ToEye(wv, p1, e1);
        if (e0[2] > -0.05f || e1[2] > -0.05f) continue;
        float sx = e1[0] - e0[0], sy = e1[1] - e0[1], sl = sqrtf(sx * sx + sy * sy) + 1e-5f;
        float nx = -sy / sl * w, ny = sx / sl * w;
        gC(kBlue[0], kBlue[1], kBlue[2], alpha);
        gB(GL_TRIANGLES);
        gV(e0[0] - nx, e0[1] - ny, e0[2]);
        gV(e1[0] - nx, e1[1] - ny, e1[2]);
        gV(e1[0] + nx, e1[1] + ny, e1[2]);
        gV(e0[0] - nx, e0[1] - ny, e0[2]);
        gV(e1[0] + nx, e1[1] + ny, e1[2]);
        gV(e0[0] + nx, e0[1] + ny, e0[2]);
        gE();
    }
}

bool InWorld() {
    const KvrShared* shm = VR_Shared();
    return shm && Weapon_PlayerCreature() && !shm->cinematic && !shm->menuOpen && !Dialogue_Showing();
}

bool Gun() {
    WeaponClass r = Weapon_Right();
    return r == WC_PISTOL || r == WC_RIFLE || r == WC_REPEATER;
}

// the left palm faces your face (the wrist HUD's gesture): the grip's palm side (+X for the left hand) toward the head
bool PalmToFace(const KvrInput& in) {
    if (!(in.gripValid & 1)) return false;
    const float* q = in.grip[0].quat;
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float px = 1 - 2 * (y * y + z * z), py = 2 * (x * y + z * w), pz = 2 * (x * z - y * w);  // rotate (1, 0, 0)
    float head[3];
    VR_HeadPos(head);
    float dx = head[0] - in.grip[0].pos[0], dy = head[1] - in.grip[0].pos[1], dz = head[2] - in.grip[0].pos[2];
    float dl = sqrtf(dx * dx + dy * dy + dz * dz) + 1e-5f;
    return (px * dx + py * dy + pz * dz) / dl > 0.6f;
}

}  // namespace

void Hints_Init(const char* gameDir) {
    sprintf_s(g_ini, "%s\\kvr\\kvr.ini", gameDir);
    // (the old Welcome / Blaster cards' keys do not count: the cards were what did not work)
    for (int h = 0; h < H_COUNT; h++) {
        char key[32];
        sprintf_s(key, "Hint%s", kKey[h]);
        g_done[h] = GetPrivateProfileIntA("Tutorial", key, 0, g_ini) != 0;
    }
}

void Hints_Update(const KvrInput& in) {
    double t = Now();
    if (g_labelUntil > 0 && t > g_labelUntil) {
        g_labelUntil = -1;
        VR_HideText(3);
    }
    const KvrShared* shm = VR_Shared();
    // B: back - the first time a game menu opens over the world
    if (!g_done[H_BACK] && shm && shm->menuOpen && g_everInWorld) {  // (a menu in the game, not the load screen)
        static double since = -1;
        if (since < 0) since = t;
        float pos[3] = {0, -0.28f, -0.9f};
        Label(KVR_ANCHOR_HEAD, pos, kLabel[H_BACK]);
        if (t - since > 4 || (in.buttons & KVR_BTN_B)) Done(H_BACK);
    }
    if (!InWorld()) {
        g_worldSince = -1;
        return;
    }
    if (g_worldSince < 0) g_worldSince = t;
    g_everInWorld = true;
    if (t - g_worldSince < 3.0) return;  // let the scene settle first
    float pp[3];
    if (!Weapon_PlayerPos(pp)) return;
    // progress, measured from where the hint first showed (not from the spawn, which settles after the load)
    if (g_active == H_WALK || g_active == H_TURN) {
        if (!g_haveStart) {
            memcpy(g_startPos, pp, 12);
            g_startYaw = EngineCam_ViewYaw();
            g_haveStart = true;
        }
        float dx = pp[0] - g_startPos[0], dy = pp[1] - g_startPos[1];
        if (g_active == H_WALK && dx * dx + dy * dy > 2.0f * 2.0f && t - g_activeSince > 1.0) Done(H_WALK), g_haveStart = false;
        else if (g_active == H_TURN && fabsf(WrapPi(EngineCam_ViewYaw() - g_startYaw)) > 0.7f && t - g_activeSince > 1.0)
            Done(H_TURN), g_haveStart = false;
    }
    if (!g_done[H_HUD] && PalmToFace(in)) Done(H_HUD);
    DWORD hit = 0;
    float hp[3];
    static double useSeenAt = -10;  // (it lingers 1.5 s after the ray moves off: a pointer wavers)
    g_haveUse = t - useSeenAt < 1.5;
    if (!g_done[H_USE] && Pick_Hit(&hit, hp) && !Melee_IsEnemy(hit)) {
        float hx = hp[0] - pp[0], hy = hp[1] - pp[1];
        if (hx * hx + hy * hy < 5 * 5) {
            memcpy(g_usePoint, hp, 12);
            float op[3];  // a person: the brackets frame their chest, not where the ray happened to touch
            if (!strcmp(Pick_HitKind(), "creature") && Weapon_ObjectPos(hit, op)) g_usePoint[0] = op[0], g_usePoint[1] = op[1], g_usePoint[2] = op[2] + 1.25f;
            g_haveUse = true;
            useSeenAt = t;
            if (in.gripR > 0.7f) Done(H_USE);
        }
    }
    if (Gun() && Combat_ShotPending() && t - g_lastShotRoll > 0.3) {
        g_lastShotRoll = t;
        if (++g_shots >= 2) Done(H_SHOOT);
    }
    if (!g_done[H_BRACE] && Gun() && Weapon_Foregrip()) Done(H_BRACE);
    // which one shows: the most relevant not yet done (fights first, then the basics in order)
    int want = -1;
    float e[3];
    bool enemy = Melee_NearestEnemy(pp, e) != 0;
    DWORD dmgOn;
    float dmgAge;
    if (!g_done[H_DAMAGE] && Weapons_LastDamage(&dmgOn, &dmgAge) && dmgAge < 2.0f) want = H_DAMAGE;
    else if (Gun() && enemy && !g_done[H_SHOOT]) want = H_SHOOT;
    else if (Gun() && enemy && g_shots >= 3 && !g_done[H_BRACE]) want = H_BRACE;
    static double combatAt = -10;  // KotOR's in-combat flag flickers: hold it 3 s
    if (Weapon_PlayerInCombat()) combatAt = t;
    if (want >= 0) {
    } else if (t - combatAt > 3.0) {
        if (!g_done[H_WALK]) want = H_WALK;
        else if (!g_done[H_TURN]) want = H_TURN;
        else if (!g_done[H_USE] && g_haveUse) want = H_USE;
        else if (!g_done[H_HUD] && t - g_worldSince > 20) want = H_HUD;
    }
    if (want == H_DAMAGE && dmgAge > 1.9f) Done(H_DAMAGE);
    if (want != g_active) {
        g_active = want;
        g_activeSince = t;
        if (want >= 0) Log("hints: showing %s", kKey[want]);
    }
    if (g_active < 0) return;
    // the label, where it belongs
    switch (g_active) {
        case H_WALK: case H_TURN: {
            float pos[3] = {0, -0.3f, -1.1f};
            Label(KVR_ANCHOR_HEAD, pos, kLabel[g_active]);
            break;
        }
        case H_HUD: {
            float pos[3] = {0.0f, 0.06f, -0.08f};  // just over the left wrist
            Label(KVR_ANCHOR_LWRIST, pos, kLabel[g_active]);
            break;
        }
        case H_BRACE: {
            float pos[3] = {0.0f, 0.08f, -0.05f};
            Label(KVR_ANCHOR_LWRIST, pos, kLabel[g_active]);
            break;
        }
        case H_USE: {
            float w[3] = {g_usePoint[0], g_usePoint[1], g_usePoint[2] + 0.35f}, pos[3];
            if (EngineCam_WorldToTracking(w, pos)) Label(KVR_ANCHOR_WORLD, pos, kLabel[g_active]);
            break;
        }
        case H_SHOOT: case H_DAMAGE: {
            DWORD id = g_active == H_DAMAGE ? dmgOn : Melee_NearestEnemy(pp, e);
            float cp[3];
            if (id && Weapon_ObjectPos(id, cp)) {
                float w[3] = {cp[0], cp[1], cp[2] + 2.1f}, pos[3];
                if (EngineCam_WorldToTracking(w, pos)) Label(KVR_ANCHOR_WORLD, pos, kLabel[g_active]);
            }
            break;
        }
    }
}

bool Hints_Drawing() { return g_active >= 0; }

void Hints_Draw(const float wv[16]) {
    if (g_active < 0 || !Gl()) return;
    gBF(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);  // the warnings pass is additive: on KotOR's white floors that vanished
    double t = Now();
    float pp[3];
    if (!Weapon_PlayerPos(pp)) return;
    // your view's heading on the floor: the eye's -Z in the world
    float f[2] = {-wv[0 * 4 + 2], -wv[1 * 4 + 2]};
    float fl = sqrtf(f[0] * f[0] + f[1] * f[1]) + 1e-5f;
    f[0] /= fl, f[1] /= fl;
    float floorZ = pp[2] + 0.03f;
    switch (g_active) {
        case H_WALK:  // chevrons flowing forward along the floor
            for (int i = 0; i < 5; i++) {
                float d = 0.8f + i * 0.45f + (float)fmod(t * 0.9, 0.45);
                float a = 0.6f * (1.0f - (d - 0.8f) / 2.3f);
                float p[3] = {pp[0] + f[0] * d, pp[1] + f[1] * d, floorZ};
                Chevron(wv, p, f, 0.22f, a);
            }
            break;
        case H_TURN: {  // chevrons running round you to the right
            float base = atan2f(f[1], f[0]);
            for (int i = 0; i < 6; i++) {  // (2.4 m out, in front and to the right: within your view)
                float an = base - (0.05f + i * 0.2f + (float)fmod(t * 0.5, 0.2));
                float p[3] = {pp[0] + cosf(an) * 2.4f, pp[1] + sinf(an) * 2.4f, floorZ};
                float dir[2] = {sinf(an), -cosf(an)};  // clockwise tangent
                Chevron(wv, p, dir, 0.26f, 0.6f * (1.0f - i / 6.0f));
            }
            break;
        }
        case H_USE: {  // brackets on what you point at, breathing
            float s = 0.4f + 0.05f * (float)sin(t * 5);
            float c[3] = {g_usePoint[0], g_usePoint[1], g_usePoint[2]};
            float r[2] = {f[1], -f[0]};
            for (int sx = -1; sx <= 1; sx += 2)
                for (int sz = -1; sz <= 1; sz += 2) {
                    float corner[3] = {c[0] + r[0] * s * sx, c[1] + r[1] * s * sx, c[2] + s * sz};
                    float h[3] = {corner[0] - r[0] * s * sx * 0.4f, corner[1] - r[1] * s * sx * 0.4f, corner[2]};
                    float v[3] = {corner[0], corner[1], corner[2] - s * sz * 0.4f};
                    Line(wv, corner, h, 0.018f, 0.9f, false);
                    Line(wv, corner, v, 0.018f, 0.9f, false);
                }
            break;
        }
        case H_SHOOT: {  // a dashed line from your barrel to its chest
            float o[3], d[3], e[3];
            DWORD id = Melee_NearestEnemy(pp, e);
            if (id && Weapons_Barrel(o, d)) {
                float c[3] = {e[0], e[1], e[2] + 1.2f};
                Line(wv, o, c, 0.012f, 0.6f + 0.3f * (float)sin(t * 6), true);
            }
            break;
        }
        case H_BRACE: {  // from your left hand to the barrel
            float o[3], d[3], lh[3];
            if (Weapons_Barrel(o, d) && Weapons_LeftHand(lh)) {
                float bar[3] = {o[0] - d[0] * 0.25f, o[1] - d[1] * 0.25f, o[2] - d[2] * 0.25f};
                Line(wv, lh, bar, 0.01f, 0.7f, true);
            }
            break;
        }
        default:
            break;
    }
    gBF(GL_SRC_ALPHA, GL_ONE);
}
