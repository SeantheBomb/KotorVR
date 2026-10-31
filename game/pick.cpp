// What the right hand's ray actually points at (2026-10-30 playtest: pointing at a chest at arm's length, the grip
// opened the door across the room / talked to Trask).
//
// The cursor used to be placed on the point 10 m down the hand's ray, seen from the head: fine far away, but the hand is
// ~0.5 m from the eyes, so a chest at arm's length got a cursor 20-30 deg off it - on whatever stood behind. Now the ray
// is tested against the area's objects (CSWSArea game object ids at +0x190 / count +0x194, Lane's labels; server
// objects by id 0x4AE750, type at +8, position +0x90), each as a vertical capsule sized by its type; the nearest one in
// line of sight wins, and the cursor goes on the point of the ray inside it - the game's own hover then resolves the
// object you see there (usable or not stays the game's call). input.cpp only acts on a hover the ray touches.
#include "pick.h"

#include <cmath>

#include "anim.h"
#include "melee.h"
#include "weapons.h"

extern void Log(const char* fmt, ...);

namespace {

float g_o[3], g_d[3];
DWORD g_aimAt;  // GetTickCount of the last aim
DWORD g_hitId;
float g_hitPoint[3];
bool g_hit;
DWORD g_touch[16];
int g_touchN;

enum { T_CREATURE = 5, T_ITEM = 6, T_PLACEABLE = 9, T_DOOR = 10 };

float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Closest approach of the ray segment o + d*t (t in [t0, t1], |d| = 1) and the vertical segment base..base+h:
// returns the distance, and t of the closest point on the ray.
float RayCapsule(const float* o, const float* d, float t0, float t1, const float* base, float h, float* tOut) {
    // parametrize the vertical segment as base + (0,0,1)*s, s in [0, h]; solve the 2x2 least squares then clamp
    float w[3] = {o[0] - base[0], o[1] - base[1], o[2] - base[2]};
    float b = d[2];  // d . up
    float dd = Dot(d, w), e = w[2];
    float den = 1.0f - b * b;
    float t = den > 1e-5f ? (b * e - dd) / den : -dd;
    t = t < t0 ? t0 : t > t1 ? t1 : t;
    float s = w[2] + d[2] * t;  // up . (ray point - base)
    s = s < 0 ? 0 : s > h ? h : s;
    // re-solve t for the clamped s
    float q[3] = {base[0] - o[0], base[1] - o[1], base[2] + s - o[2]};
    t = Dot(q, d);
    t = t < t0 ? t0 : t > t1 ? t1 : t;
    float p[3] = {o[0] + d[0] * t - base[0], o[1] + d[1] * t - base[1], o[2] + d[2] * t - base[2] - s};
    *tOut = t;
    return sqrtf(Dot(p, p));
}

// Closest approach of the ray segment o + d*t (t in [t0, t1]) and the segment a..b: distance, and t on the ray.
float RaySeg(const float* o, const float* d, float t0, float t1, const float* a, const float* b, float* tOut) {
    float p0[3] = {o[0] + d[0] * t0, o[1] + d[1] * t0, o[2] + d[2] * t0};
    float u[3] = {d[0] * (t1 - t0), d[1] * (t1 - t0), d[2] * (t1 - t0)}, v[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    float w[3] = {p0[0] - a[0], p0[1] - a[1], p0[2] - a[2]};
    float A = Dot(u, u), B = Dot(u, v), C = Dot(v, v), D = Dot(u, w), E = Dot(v, w), den = A * C - B * B;
    float sc = den > 1e-8f ? (B * E - C * D) / den : 0, tc;
    sc = sc < 0 ? 0 : sc > 1 ? 1 : sc;
    tc = C > 1e-8f ? (B * sc + E) / C : 0;
    if (tc < 0 || tc > 1) {
        tc = tc < 0 ? 0 : 1;
        sc = A > 1e-8f ? (B * tc - D) / A : 0;
        sc = sc < 0 ? 0 : sc > 1 ? 1 : sc;
    }
    float q[3];
    for (int k = 0; k < 3; k++) q[k] = w[k] + u[k] * sc - v[k] * tc;
    *tOut = t0 + (t1 - t0) * sc;
    return sqrtf(Dot(q, q));
}

}  // namespace

void Pick_SetAim(const float origin[3], const float dir[3]) {
    float l = sqrtf(Dot(dir, dir));
    if (l < 1e-4f) return;
    for (int i = 0; i < 3; i++) {
        g_o[i] = origin[i];
        g_d[i] = dir[i] / l;
    }
    g_aimAt = GetTickCount();
}

bool Pick_Active() { return g_aimAt && GetTickCount() - g_aimAt < 300; }

void Pick_Update() {
    g_hit = false;
    g_touchN = 0;
    if (!Pick_Active()) return;
    typedef void*(__fastcall * GetAreaFn)(void* obj, void*);
    typedef void*(__thiscall * GetObject_t)(void* server, DWORD id);
    struct Cand {
        DWORD id;
        float t, center[3];
    } cand[16];
    int nc = 0;
    DWORD playerId = 0;
    __try {
        BYTE* player = (BYTE*)Weapon_PlayerCreature();
        if (!player) return;
        playerId = *(DWORD*)(player + 4);
        BYTE* area = (BYTE*)((GetAreaFn)0x4CB120)(player, nullptr);
        BYTE* app = *(BYTE**)0x7A39FC;
        BYTE* server = app ? *(BYTE**)(app + 8) : nullptr;
        if (!area || !server) return;
        DWORD* ids = *(DWORD**)(area + 0x190);
        int n = *(int*)(area + 0x194);
        if (!ids || n <= 0 || n > 8192) return;
        static int loggedN = -1;
        if (n != loggedN) {
            loggedN = n;
            Log("pick: %d objects in the area", n);
        }
        for (int i = 0; i < n; i++) {
            DWORD id = ids[i];
            if (id == playerId || id == 0x7F000000) continue;
            BYTE* o = (BYTE*)((GetObject_t)0x4AE750)(server, id);
            if (!o) continue;
            int type = *(int*)(o + 8);
            float r, h, lift = 0;
            switch (type) {
                case T_CREATURE: r = 0.45f; h = 1.9f; break;
                case T_PLACEABLE: r = 0.55f; h = 1.1f; break;
                case T_ITEM: r = 0.3f; h = 0.3f; break;
                case T_DOOR: r = 1.1f; h = 2.6f; break;
                default: continue;
            }
            const float* pos = (const float*)(o + 0x90);
            float dx = pos[0] - g_o[0], dy = pos[1] - g_o[1];
            if (dx * dx + dy * dy > 40.0f * 40.0f) continue;
            float base[3] = {pos[0], pos[1], pos[2] + lift}, t;
            float dist = 1e9f;
            if (type == T_CREATURE) {
                // creatures: their skeleton as drawn (lying corpses, droids, kinrath legs), else an upright capsule
                // (GetDead also says yes for some live props - the Dantooine training droid - so it decides nothing)
                AnimBone bones[96];
                void* model = Anim_ModelAt(pos);
                int nb = model ? Anim_BoneSegments(model, bones, 96) : 0;
                if (nb > 3) {
                    for (int k = 0; k < nb; k++) {
                        float bt, bd = RaySeg(g_o, g_d, 0.15f, 40.0f, bones[k].a, bones[k].b, &bt) - bones[k].radius;
                        if (bd < dist) {
                            dist = bd;
                            t = bt;
                        }
                    }
                    r = 0.08f;  // margin around the bones
                }
            }
            if (dist > 1e8f) dist = RayCapsule(g_o, g_d, 0.15f, 40.0f, base, h, &t);
            // a little cone so small things and far ones are still easy to point at
            if (dist > r + 0.035f * t) continue;
            if (nc < 16) {
                cand[nc].id = id;
                cand[nc].t = t;
                cand[nc].center[0] = pos[0];
                cand[nc].center[1] = pos[1];
                cand[nc].center[2] = pos[2] + h * 0.5f;
                nc++;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    // nearest first; the first one in line of sight from the hand is the pick, everything touched counts as touched
    for (int i = 1; i < nc; i++)
        for (int j = i; j > 0 && cand[j].t < cand[j - 1].t; j--) {
            Cand c = cand[j];
            cand[j] = cand[j - 1];
            cand[j - 1] = c;
        }
    for (int i = 0; i < nc; i++) {
        g_touch[g_touchN++] = cand[i].id;
        if (g_hit) continue;
        if (!Melee_LineOfSight(g_o, cand[i].center, playerId, cand[i].id)) {
            static DWORD lastBlocked;
            if (cand[i].id != lastBlocked) {
                lastBlocked = cand[i].id;
                Log("pick: %08lX on the ray at %.1f m but out of sight", cand[i].id, cand[i].t);
            }
            continue;
        }
        g_hit = true;
        g_hitId = cand[i].id;
        for (int k = 0; k < 3; k++) g_hitPoint[k] = g_o[k] + g_d[k] * cand[i].t;
    }
    static DWORD lastLogged;
    if (g_hit && g_hitId != lastLogged) {
        lastLogged = g_hitId;
        Log("pick: ray on %08lX at %.1f m", g_hitId, sqrtf((g_hitPoint[0] - g_o[0]) * (g_hitPoint[0] - g_o[0]) +
                                                          (g_hitPoint[1] - g_o[1]) * (g_hitPoint[1] - g_o[1]) +
                                                          (g_hitPoint[2] - g_o[2]) * (g_hitPoint[2] - g_o[2])));
    }
}

bool Pick_Hit(DWORD* serverId, float point[3]) {
    if (!g_hit) return false;
    *serverId = g_hitId;
    for (int i = 0; i < 3; i++) point[i] = g_hitPoint[i];
    return true;
}

bool Pick_Touches(DWORD serverId) {
    if (!Pick_Active()) return true;
    serverId &= 0x7FFFFFFF;
    for (int i = 0; i < g_touchN; i++)
        if (g_touch[i] == serverId) return true;
    return false;
}
