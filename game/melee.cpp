// v2 contact combat core (docs/V2_PROPOSAL.md pillars 1-3): your blade in the world, who is fighting whom, blade /
// bolt contacts, and the "credits" that let KotOR's d20 rounds land only what your hands actually did.
//
// Blade: the drawn weapon's grip -> tip segment, converted from eye space to KotOR world space every frame by the
// weapon pass (Melee_SetBlade), with its tip velocity.
// Sides: anyone who rolls an attack on you is an enemy; whoever attacks an enemy is an ally; whoever an ally attacks
// is an enemy (no faction lookup needed). Contacts only count against enemies.
// Contact: the blade segment (and its sweep since last frame) within a body capsule (feet + 0.25 .. + 1.75 m, radius
// 0.34 m) with tip speed >= 2.0 m/s (JKXR's gate), re-armed below 1.2 m/s, at most once per enemy per 0.35 s.
// Credit: each contact / aimed shot hit credits that enemy for 1.6 s; combat.cpp lets the player's next landing
// blow / bolt on it through (graded by the d20 and the swing speed) and turns uncredited ones into misses.
#include "melee.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <map>
#include <set>

#include "anim.h"
#include "cues.h"
#include "weapons.h"

extern void Log(const char* fmt, ...);

namespace {

double Now() {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / f.QuadPart;
}

struct Blade {
    bool valid;
    float base[3], tip[3], prevBase[3], prevTip[3];
    double t, prevT;
    float speed;     // tip speed, m/s (smoothed)
    float maxRecent; // highest tip speed in the last 0.3 s
    double maxAt;
    bool armed = true;
};
Blade g_blade[2];

struct Fighter {
    double seen;
    bool enemy, ally;
};
std::map<DWORD, Fighter> g_fighters;

struct Credit {
    double at;
    float speed;   // swing speed (1 for shots)
    bool head;     // a head hit (shots)
};
std::map<DWORD, Credit> g_credit;
std::map<DWORD, double> g_lastContact;

float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// closest distance between segments p0-p1 and q0-q1, and the closest point on the first (Ericson, RTCD 5.1.9)
float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
float SegSeg(const float* p0, const float* p1, const float* q0, const float* q1, float* onP) {
    float d1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, d2[3] = {q1[0] - q0[0], q1[1] - q0[1], q1[2] - q0[2]};
    float r[3] = {p0[0] - q0[0], p0[1] - q0[1], p0[2] - q0[2]};
    float a = Dot(d1, d1), e = Dot(d2, d2), f = Dot(d2, r), s, t;
    const float eps = 1e-8f;
    if (a <= eps && e <= eps) {
        s = t = 0;
    } else if (a <= eps) {
        s = 0;
        t = Clamp01(f / e);
    } else {
        float c = Dot(d1, r);
        if (e <= eps) {
            t = 0;
            s = Clamp01(-c / a);
        } else {
            float b = Dot(d1, d2), den = a * e - b * b;
            s = den > eps ? Clamp01((b * f - c * e) / den) : 0;
            t = (b * s + f) / e;
            if (t < 0) {
                t = 0;
                s = Clamp01(-c / a);
            } else if (t > 1) {
                t = 1;
                s = Clamp01((b - c) / a);
            }
        }
    }
    float cp[3] = {p0[0] + d1[0] * s, p0[1] + d1[1] * s, p0[2] + d1[2] * s};
    float cq[3] = {q0[0] + d2[0] * t, q0[1] + d2[1] * t, q0[2] + d2[2] * t};
    if (onP) memcpy(onP, cp, 12);
    float w[3] = {cp[0] - cq[0], cp[1] - cq[1], cp[2] - cq[2]};
    return sqrtf(Dot(w, w));
}

bool CreaturePos(DWORD id, float out[3]) {
    __try {
        BYTE* c = (BYTE*)Weapon_CreatureById(id);
        if (!c) return false;
        memcpy(out, c + 0x90, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

DWORD PlayerId() {
    __try {
        BYTE* p = (BYTE*)Weapon_PlayerCreature();
        return p ? *(DWORD*)(p + 4) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

}  // namespace

void Melee_SetBlade(int hand, const float base[3], const float tip[3]) {
    if (hand < 0 || hand > 1) return;
    Blade& b = g_blade[hand];
    double t = Now();
    if (b.valid && t - b.t < 0.004) return;  // the other eye of the same frame
    memcpy(b.prevBase, b.valid ? b.base : base, 12);
    memcpy(b.prevTip, b.valid ? b.tip : tip, 12);
    b.prevT = b.valid ? b.t : t;
    memcpy(b.base, base, 12);
    memcpy(b.tip, tip, 12);
    b.t = t;
    double dt = b.t - b.prevT;
    if (b.valid && dt > 0.002 && dt < 0.2) {
        float v[3] = {b.tip[0] - b.prevTip[0], b.tip[1] - b.prevTip[1], b.tip[2] - b.prevTip[2]};
        float sp = sqrtf(Dot(v, v)) / (float)dt;
        b.speed = b.speed * 0.4f + sp * 0.6f;
        if (b.speed > b.maxRecent || t - b.maxAt > 0.3) {
            b.maxRecent = b.speed;
            b.maxAt = t;
        }
    }
    b.valid = true;
}

float Melee_BladeSpeed(int hand) {
    if (hand < 0 || hand > 1 || !g_blade[hand].valid || Now() - g_blade[hand].t > 0.2) return 0;
    return g_blade[hand].speed;
}

void Melee_ClearBlade(int hand) {
    if (hand >= 0 && hand <= 1) g_blade[hand].valid = false;
}

void Melee_NoteAttack(DWORD attacker, DWORD target) {
    DWORD me = PlayerId();
    if (!attacker || !target || !me) return;
    double t = Now();
    Fighter& a = g_fighters[attacker];
    Fighter& b = g_fighters[target];
    a.seen = b.seen = t;
    if (target == me) a.enemy = true;
    if (attacker == me) b.enemy = true;
    if (b.enemy && attacker != me) a.ally = true;   // attacks an enemy: on your side
    if (a.ally && target != me) b.enemy = true;     // an ally's target: an enemy
    if (a.enemy && target != me) b.ally = true;     // an enemy's target: on your side
}

// The creature's faction (CSWSCreature::GetFaction 0x513FC0: [[creature + 0xA74] + 0x78]); -1 unknown.
static int FactionOf(DWORD id) {
    __try {
        BYTE* c = (BYTE*)Weapon_CreatureById(id);
        BYTE* stats = c ? *(BYTE**)(c + 0xA74) : nullptr;
        return stats ? *(int*)(stats + 0x78) : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}

bool Melee_IsEnemy(DWORD id) {
    auto it = g_fighters.find(id);
    if (it == g_fighters.end() || Now() - it->second.seen > 30) return false;
    if (it->second.ally) return false;
    if (it->second.enemy) return true;
    // in a fight but not (yet) in one with you: a hostile faction (STANDARD_FACTION_HOSTILE_1 / _2, INSANE, PTAT_TUSKAN,
    // PREDATOR, RANCOR - nwscript.nss) counts, so the first cut lands before it has swung at you
    int f = FactionOf(id);
    return f == 1 || f == 3 || f == 6 || f == 7 || f == 11 || f == 15;
}

bool Melee_TakeCredit(DWORD target, float* speed, bool* head) {
    auto it = g_credit.find(target);
    if (it == g_credit.end() || Now() - it->second.at > 1.6) return false;
    if (speed) *speed = it->second.speed;
    if (head) *head = it->second.head;
    g_credit.erase(it);
    return true;
}

bool Melee_HasCredit(DWORD target) {
    auto it = g_credit.find(target);
    return it != g_credit.end() && Now() - it->second.at <= 1.6;
}

static DWORD g_startAttackOn;
DWORD Melee_TakeAttackStart() {
    DWORD id = g_startAttackOn;
    g_startAttackOn = 0;
    return id;
}

static void Contact(DWORD id, const float point[3], float speed) {
    double t = Now();
    g_lastContact[id] = t;
    g_credit[id] = {t, speed, false};
    Anim_HitPause(id, speed > 5.0f ? 0.09f : 0.06f);
    Anim_Flinch(id, point);
    Cues_Contact(speed);
    if (!g_startAttackOn) g_startAttackOn = id;  // if you aren't fighting it yet, this starts the attack
    Log("melee: blade contact on %08lX at %.1f m/s", id, speed);
}

void Melee_Update() {
    double t = Now();
    // forget stale fighters / credits
    for (auto it = g_fighters.begin(); it != g_fighters.end();)
        it = t - it->second.seen > 40 ? g_fighters.erase(it) : std::next(it);
    static double lastDiag;
    if (t - lastDiag > 2.0) {  // diagnostics: blade state and the nearest enemy
        lastDiag = t;
        const Blade& rb = g_blade[1];
        float best = 99;
        int enemies = 0;
        for (auto& kv : g_fighters) {
            if (!Melee_IsEnemy(kv.first)) continue;
            enemies++;
            float p[3];
            if (!CreaturePos(kv.first, p) || !rb.valid) continue;
            float dx = p[0] - rb.tip[0], dy = p[1] - rb.tip[1];
            float d = sqrtf(dx * dx + dy * dy);
            if (d < best) best = d;
        }
        int bonesOfNearest = -1;
        for (auto& kv : g_fighters) {
            float p[3];
            if (Melee_IsEnemy(kv.first) && CreaturePos(kv.first, p)) {
                AnimBone bb[96];
                void* m = Anim_ModelAt(p);
                bonesOfNearest = m ? Anim_BoneSegments(m, bb, 96) : 0;
                break;
            }
        }
        if (enemies || rb.valid)
            Log("melee: blade %s speed %.1f m/s tip (%.1f %.1f %.1f), %d enemies, nearest %.1f m (fighters %d, bones %d)",
                rb.valid ? "on" : "off", rb.speed, rb.tip[0], rb.tip[1], rb.tip[2], enemies, best, (int)g_fighters.size(),
                bonesOfNearest);
    }
    for (int h = 0; h < 2; h++) {
        Blade& b = g_blade[h];
        if (!b.valid || t - b.t > 0.2) continue;
        if (b.speed < 1.2f) b.armed = true;
        if (b.speed < 2.0f || !b.armed) continue;
        for (auto& kv : g_fighters) {
            if (!Melee_IsEnemy(kv.first)) continue;
            float p[3];
            if (!CreaturePos(kv.first, p)) continue;
            float hit[3];
            bool touched = false;
            void* model = Anim_ModelAt(p);
            AnimBone bones[96];
            int nb = model ? Anim_BoneSegments(model, bones, 96) : 0;
            if (nb > 3) {  // per-bone capsules: exact for any creature shape (kinrath legs, droids, humans)
                for (int k = 0; k < nb && !touched; k++) {
                    float d = SegSeg(b.base, b.tip, bones[k].a, bones[k].b, hit);
                    float sweep = SegSeg(b.prevTip, b.tip, bones[k].a, bones[k].b, nullptr);
                    if (d <= bones[k].radius + 0.03f || sweep <= bones[k].radius) touched = true;
                }
            } else {  // no skeleton found: a body capsule
                float lo[3] = {p[0], p[1], p[2] + 0.25f}, hi[3] = {p[0], p[1], p[2] + 1.75f};
                float d = SegSeg(b.base, b.tip, lo, hi, hit);
                float sweep = SegSeg(b.prevTip, b.tip, lo, hi, nullptr);  // the tip's path since last frame
                touched = d <= 0.34f || sweep <= 0.34f;
            }
            if (!touched) continue;
            auto lc = g_lastContact.find(kv.first);
            if (lc != g_lastContact.end() && t - lc->second < 0.35) continue;
            b.armed = false;
            Contact(kv.first, hit, b.speed);
            break;
        }
    }
}

// --- defence: the blade against a strike zone / a bolt's path
bool Melee_BladeNear(const float point[3], float radius, float* speed) {
    for (int h = 0; h < 2; h++) {
        const Blade& b = g_blade[h];
        if (!b.valid || Now() - b.t > 0.2) continue;
        float q[3] = {point[0], point[1], point[2]};
        if (SegSeg(b.base, b.tip, q, q, nullptr) <= radius) {
            if (speed) *speed = (Now() - b.maxAt < 0.3) ? b.maxRecent : b.speed;
            return true;
        }
    }
    return false;
}

bool Melee_BladeCrossesLine(const float a[3], const float b2[3], float radius, float* speed) {
    for (int h = 0; h < 2; h++) {
        const Blade& b = g_blade[h];
        if (!b.valid || Now() - b.t > 0.2) continue;
        if (SegSeg(b.base, b.tip, a, b2, nullptr) <= radius) {
            if (speed) *speed = (Now() - b.maxAt < 0.3) ? b.maxRecent : b.speed;
            return true;
        }
    }
    return false;
}

// --- Force: the enemy the left hand points at
static float g_palmO[3], g_palmD[3];
static double g_palmAt = -10;
void Melee_SetPalm(const float origin[3], const float dir[3]) {
    memcpy(g_palmO, origin, 12);
    memcpy(g_palmD, dir, 12);
    g_palmAt = Now();
}

DWORD Melee_PalmTarget() {
    {  // diagnostics (rate-limited): what the palm aim sees
        static double last;
        if (Now() - last > 1.0) {
            last = Now();
            Log("melee: palm age %.2f s, origin (%.1f %.1f %.1f) dir (%.2f %.2f %.2f)", Now() - g_palmAt, g_palmO[0], g_palmO[1],
                g_palmO[2], g_palmD[0], g_palmD[1], g_palmD[2]);
            for (auto& kv : g_fighters) {
                float p[3];
                if (Melee_IsEnemy(kv.first) && CreaturePos(kv.first, p))
                    Log("melee:   enemy %08lX at (%.1f %.1f %.1f)", kv.first, p[0], p[1], p[2]);
            }
        }
    }
    if (Now() - g_palmAt > 0.3) return 0;
    DWORD best = 0;
    float bestCos = cosf(35.0f * 3.14159265f / 180.0f);
    for (auto& kv : g_fighters) {
        if (!Melee_IsEnemy(kv.first)) continue;
        float p[3];
        if (!CreaturePos(kv.first, p)) continue;
        float v[3] = {p[0] - g_palmO[0], p[1] - g_palmO[1], p[2] + 1.1f - g_palmO[2]};
        float l = sqrtf(Dot(v, v));
        if (l < 0.3f || l > 15.0f) continue;
        float c = Dot(v, g_palmD) / l;
        if (c > bestCos) {
            bestCos = c;
            best = kv.first;
        }
    }
    if (best) return best;
    // forgiving fallbacks: the enemy the right hand points at, else the nearest enemy in front of the palm (horizontal
    // 60 deg, 8 m)
    DWORD hover;
    if (Weapon_HoverClientId(&hover) && Melee_IsEnemy(hover & 0x7FFFFFFF)) return hover & 0x7FFFFFFF;
    float bestD = 8.0f;
    float hl = sqrtf(g_palmD[0] * g_palmD[0] + g_palmD[1] * g_palmD[1]) + 1e-5f;
    for (auto& kv : g_fighters) {
        if (!Melee_IsEnemy(kv.first)) continue;
        float p[3];
        if (!CreaturePos(kv.first, p)) continue;
        float vx = p[0] - g_palmO[0], vy = p[1] - g_palmO[1], l = sqrtf(vx * vx + vy * vy);
        if (l < 0.3f || l > bestD) continue;
        if ((vx * g_palmD[0] + vy * g_palmD[1]) / (l * hl) < 0.5f) continue;
        bestD = l;
        best = kv.first;
    }
    return best;
}

// the enemy nearest a point this far out along the palm's heading (a thrown grenade's landing spot), within radius
DWORD Melee_LandingTarget(float range, float radius) {
    if (Now() - g_palmAt > 0.5) return 0;
    float hl = sqrtf(g_palmD[0] * g_palmD[0] + g_palmD[1] * g_palmD[1]) + 1e-5f;
    float land[2] = {g_palmO[0] + g_palmD[0] / hl * range, g_palmO[1] + g_palmD[1] / hl * range};
    DWORD best = 0;
    float bd = radius;
    for (auto& kv : g_fighters) {
        if (!Melee_IsEnemy(kv.first)) continue;
        float p[3];
        if (!CreaturePos(kv.first, p)) continue;
        float d = sqrtf((p[0] - land[0]) * (p[0] - land[0]) + (p[1] - land[1]) * (p[1] - land[1]));
        if (d < bd) {
            bd = d;
            best = kv.first;
        }
    }
    return best;
}

// --- ranged: the barrel ray against enemy bodies
void Melee_Shot(const float origin[3], const float dir[3]) {
    float best = 40.0f;
    DWORD bestId = 0;
    bool head = false;
    float end[3] = {origin[0] + dir[0] * 40, origin[1] + dir[1] * 40, origin[2] + dir[2] * 40};
    for (auto& kv : g_fighters) {
        if (!Melee_IsEnemy(kv.first)) continue;
        float p[3];
        if (!CreaturePos(kv.first, p)) continue;
        void* model = Anim_ModelAt(p);
        AnimBone bones[96];
        int nb = model ? Anim_BoneSegments(model, bones, 96) : 0;
        if (nb > 3) {  // per-bone: the bolt hits the limb it passes through; head bones = a head shot
            for (int k = 0; k < nb; k++) {
                float on[3];
                float d = SegSeg(origin, end, bones[k].a, bones[k].b, on);
                if (d > bones[k].radius + 0.04f) continue;
                float r[3] = {on[0] - origin[0], on[1] - origin[1], on[2] - origin[2]};
                float dist = sqrtf(Dot(r, r));
                if (dist < best) {
                    best = dist;
                    bestId = kv.first;
                    head = bones[k].head;
                }
            }
            continue;
        }
        float lo[3] = {p[0], p[1], p[2] + 0.2f}, hi[3] = {p[0], p[1], p[2] + 1.8f};
        float on[3];
        float d = SegSeg(origin, end, lo, hi, on);
        if (d > 0.36f) continue;
        float r[3] = {on[0] - origin[0], on[1] - origin[1], on[2] - origin[2]};
        float dist = sqrtf(Dot(r, r));
        if (dist < best) {
            best = dist;
            bestId = kv.first;
            head = on[2] > p[2] + 1.45f;
        }
    }
    if (!bestId) {
        Log("melee: shot hit no enemy");
        return;
    }
    g_credit[bestId] = {Now(), 1.0f, head};
    Anim_HitPause(bestId, head ? 0.09f : 0.05f);
    float hit[3] = {origin[0] + dir[0] * best, origin[1] + dir[1] * best, origin[2] + dir[2] * best};
    Anim_Flinch(bestId, hit);
    Log("melee: shot on %08lX at %.1f m%s", bestId, best, head ? " (head)" : "");
}
