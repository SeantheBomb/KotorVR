// Choreographed combat (the combat design canvas, 2026-11-12: "play the choreography").
//
// KotOR rolls every attack at the start of a 3 s combat round and picks a paired animation for both fighters
// (combatanimations.2da); the blow lands on the attack animation's Hit event, 0.5-1.3 s after the roll, and the
// defender's own parry / dodge / damage animation is timed to meet it. So the moment the roll is written we know who
// swings, which animation, when it lands and where its blade will be. This file turns that into gates:
//
//   enemy gate   the attacker's blade at its Hit (from the choreography table, kvr\choreo.txt, exported from the
//                game's own animations by tools/choreo_export.py), placed by its model's transform: meet it with your
//                blade to parry (guard.cpp judges it, at any crossing angle)
//   your gate    your hidden body's blade at the Hit of the attack the round gave you: cut through it on its ring and
//                the blow lands clean, in sync with the enemy's own reaction animation; late or off-line = glancing;
//                not swung = no hit (damage cancelled, its flinch swapped for a parry)
//   charge glow  a beat (0.75 s) before an enemy's round rolls (its round timer), so the order of blows is readable
//   breath beat  the 0.75 s before your own roll: hold a stance (ghost blades show them) and the real feat is queued
//                through KotOR's own action menu - Power Attack, Critical Strike or Flurry with every rule of the feat
//
// The d20 keeps hit chance (levels and Defense keep their meaning): a perfect gate never turns a dice miss into a hit,
// it turns it into posture (blade on blade with the enemy's parry). Every outcome is one event that drives every cue,
// led by the enemy's body (its native reaction, a hit-pause, a stagger), so cues cannot disagree.
//
// [Combat] Mode=choreo (default) | contact (v2 rules) | rounds (v1). Knobs: GateWindow (s, default 0.35), GateRadius
// (m, 0.22), IncomingDamage (1.0), AttackTokens (0 = no cap; tanking stays possible), StanceGhosts (1),
// FightGrade (1). [Debug] Bot=0..3 (simcmd "bot N"), AnimLog=1 (every model animation started, for the probes).
#include "fight.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "anim.h"
#include "combat.h"
#include "cues.h"
#include "enginecam.h"
#include "features.h"
#include "guard.h"
#include "hover.h"
#include "melee.h"
#include "simcmd.h"
#include "vr.h"
#include "weapons.h"

extern void Log(const char* fmt, ...);

// GL entry points (weapons.cpp loads them for the warnings pass; same context)
#include <gl/GL.h>

namespace {

double Now() {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / f.QuadPart;
}

float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float Len3(const float* a) { return sqrtf(Dot3(a, a)); }
void Norm3(float* a) {
    float l = Len3(a) + 1e-6f;
    a[0] /= l, a[1] /= l, a[2] /= l;
}
float Clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// quaternion (x, y, z, w) rotate
void QRot(const float q[4], const float v[3], float out[3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

float SegSeg(const float* p0, const float* p1, const float* q0, const float* q1) {
    float d1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, d2[3] = {q1[0] - q0[0], q1[1] - q0[1], q1[2] - q0[2]};
    float r[3] = {p0[0] - q0[0], p0[1] - q0[1], p0[2] - q0[2]};
    float a = Dot3(d1, d1), e = Dot3(d2, d2), f = Dot3(d2, r), s = 0, t = 0;
    if (a > 1e-8f && e > 1e-8f) {
        float c = Dot3(d1, r), b = Dot3(d1, d2), den = a * e - b * b;
        s = den > 1e-8f ? Clamp((b * f - c * e) / den, 0, 1) : 0;
        t = (b * s + f) / e;
        if (t < 0) t = 0, s = Clamp(-c / a, 0, 1);
        else if (t > 1) t = 1, s = Clamp((b - c) / a, 0, 1);
    } else if (e > 1e-8f) {
        t = Clamp(f / e, 0, 1);
    } else if (a > 1e-8f) {
        s = Clamp(-Dot3(d1, r) / a, 0, 1);
    }
    float w[3];
    for (int k = 0; k < 3; k++) w[k] = (p0[k] + d1[k] * s) - (q0[k] + d2[k] * t);
    return Len3(w);
}

// ---------------------------------------------------------------- the choreography table
struct ChoreoAnim {
    float length = 0, hit = -1, trans = 0;
    std::vector<float> s;  // per sample: t + 15 floats (rb, rt, rt2, lb, lt)
    int steps = 0;
};
std::map<std::string, ChoreoAnim> g_table;
std::map<int, std::vector<std::string>> g_react;  // reaction row -> candidate attacks
std::map<int, char> g_reactKind;                  // 'p' parry, 'g' dodge, 'd' damage
std::map<int, std::string> g_rowName;

void LoadTable(const char* gameDir) {
    char path[MAX_PATH];
    sprintf_s(path, "%s\\kvr\\choreo.txt", gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") || !f) {
        Log("fight: %s missing (tools/choreo_export.py) - gates fall back to the block bar", path);
        return;
    }
    char line[1024];
    ChoreoAnim* cur = nullptr;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'a' && !strncmp(line, "anim ", 5)) {
            char nm[32];
            ChoreoAnim a;
            if (sscanf_s(line + 5, "%31s %f %f %f %d", nm, (unsigned)sizeof nm, &a.length, &a.hit, &a.trans, &a.steps) == 5) {
                cur = &(g_table[nm] = a);
                cur->s.reserve(cur->steps * 16);
            }
        } else if (line[0] == 's' && line[1] == ' ' && cur) {
            float v[16];
            char* p = line + 2;
            int n = 0;
            for (; n < 16; n++) {
                char* e;
                v[n] = strtof(p, &e);
                if (e == p) break;
                p = e;
            }
            if (n == 16) cur->s.insert(cur->s.end(), v, v + 16);
        } else if (!strncmp(line, "react ", 6)) {
            int row;
            char atks[256], kind[4], nm[32];
            if (sscanf_s(line + 6, "%d %255s %3s %31s", &row, atks, (unsigned)sizeof atks, kind, (unsigned)sizeof kind, nm,
                         (unsigned)sizeof nm) >= 3) {
                std::vector<std::string> v;
                for (char* c = strtok(atks, ","); c; c = strtok(nullptr, ",")) v.push_back(c);
                g_react[row] = v;
                g_reactKind[row] = kind[0];
            }
        } else if (!strncmp(line, "row ", 4)) {
            int row;
            char nm[32];
            if (sscanf_s(line + 4, "%d %31s", &row, nm, (unsigned)sizeof nm) == 2) g_rowName[row] = nm;
        }
    }
    fclose(f);
    Log("fight: choreography table: %d animations, %d reaction pairs", (int)g_table.size(), (int)g_react.size());
}

// the blades of an animation at time t (model space): rb, rt, rt2, lb, lt
bool ChoreoPose(const ChoreoAnim& a, float t, float out[15]) {
    int n = (int)(a.s.size() / 16);
    if (n < 2) return false;
    float ft = Clamp(t * 30.0f, 0, (float)(n - 1));
    int i = (int)ft;
    if (i >= n - 1) i = n - 2;
    float k = ft - i;
    const float* A = &a.s[i * 16 + 1];
    const float* B = &a.s[(i + 1) * 16 + 1];
    for (int j = 0; j < 15; j++) out[j] = A[j] + (B[j] - A[j]) * k;
    return true;
}

// ---------------------------------------------------------------- model animations (Gob::PlayAnimation 0x485BD0)
struct GobAnim {
    std::string name;
    double at = -10;
    float speed = 1;
};
std::map<void*, GobAnim> g_gobAnim;
std::map<void*, double> g_swapUntil;  // defender gob: its damage reaction becomes a parry until then
bool g_animLog;
BYTE* g_playTramp;
const DWORD kPlayVA = 0x485BD0;
const BYTE kPlayOrig[6] = {0x81, 0xEC, 0x48, 0x01, 0x00, 0x00};  // sub esp, 0x148

std::string Lower(const char* s) {
    std::string r;
    for (; s && *s && r.size() < 31; s++) r += (char)tolower((unsigned char)*s);
    return r;
}

// "c2d3" -> "c2p3", "g2r2" / "g2d1" -> "g2g1": the parry / dodge that answers the same attack
const char* ParryFor(const std::string& dmg) {
    static std::map<std::string, std::string> cache;
    auto it = cache.find(dmg);
    if (it != cache.end()) return it->second.empty() ? nullptr : it->second.c_str();
    std::string r;
    if (dmg.size() == 4 && (dmg[2] == 'd' || dmg[2] == 'r')) {
        std::string c = dmg;
        c[2] = 'p';
        if (g_table.count(c)) r = c;
        else {
            c[2] = 'g';
            c[3] = '1';
            if (g_table.count(c)) r = c;
        }
    }
    cache[dmg] = r;
    return r.empty() ? nullptr : cache[dmg].c_str();
}

}  // namespace

static void PlayAnimSeen(void* gob, const char** nameSlot, const char* name, float speed) {
    {
        std::string n = Lower(name);
        double t = Now();
        // a skipped gate: the defender's damage reaction becomes the parry that answers the same attack
        auto sw = g_swapUntil.find(gob);
        if (sw != g_swapUntil.end()) {
            if (t < sw->second && n.size() == 4 && (n[2] == 'd' || n[2] == 'r')) {
                if (const char* p = ParryFor(n)) {
                    *nameSlot = p;
                    Log("fight: reaction %s swapped for %s (the blow was never swung)", n.c_str(), p);
                    n = p;
                    g_swapUntil.erase(sw);
                }
            } else if (t >= sw->second) {
                g_swapUntil.erase(sw);
            }
        }
        if (g_table.count(n)) {
            GobAnim& g = g_gobAnim[gob];
            g.name = n;
            g.at = t;
            g.speed = speed > 0.05f && speed < 5 ? speed : 1;
        }
        if (g_animLog && n.size() == 4 && n[1] >= '0' && n[1] <= '9' && n[2] != 'r') {  // combat rows only (not the ready loops)
            static int logged;
            if (logged++ < 4000) Log("anim: %p plays %s x%.2f", gob, n.c_str(), speed);
        }
    }
}

extern "C" void __cdecl Fight_PlayAnimPre(void* gob, const char** nameSlot) {
    char name[32];
    float speed = 1;
    __try {
        const char* p = *nameSlot;
        if (!p || !*p) return;
        int i = 0;
        for (; i < 31 && p[i]; i++) name[i] = p[i];
        name[i] = 0;
        speed = *(float*)(nameSlot + 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    PlayAnimSeen(gob, nameSlot, name, speed);
}

__declspec(naked) static void PlayAnimStub() {
    __asm {
        push ecx
        lea eax, [esp + 8]   // &name (the first stack argument)
        push eax
        push ecx             // the gob
        call Fight_PlayAnimPre
        add esp, 8
        pop ecx
        jmp dword ptr [g_playTramp]
    }
}

namespace {

// ---------------------------------------------------------------- settings
bool g_active = true;
float g_window = 0.35f;     // seconds before the Hit a cut through the gate counts
float g_radius = 0.22f;     // metres from the gate's blade
float g_incoming = 1.0f;
float g_abBonus = 0;        // progression: + gate radius from your attack bonus (m)
bool g_ghosts = true;
bool g_grade = true;
int g_bot;
char g_statsIni[MAX_PATH];
int g_fightsSeen;           // fights so far (stance ghosts fade after a few)
double g_gradeUntil = -1;   // the exchange grade panel (text slot 3) comes down then

// ---------------------------------------------------------------- gates
struct Gate {
    DWORD attacker, target;
    bool mine;           // your blow (else an enemy's on you)
    BYTE* record;
    double rolled;
    bool read = false;   // the record has been read (a frame after the roll)
    int dice = 0;        // record +0x5C after the roll: 1 hit, 4 miss, 8..10 parried / deflected
    int react = -1;      // +0x12
    WORD feat = 0;       // +0x64
    std::string anim;
    const ChoreoAnim* ca = nullptr;
    double start = 0, due = -1;
    float poseT = 0;     // the animation's time at the blow (its Hit, less a late start)
    float speed = 1;
    float a[3], b[3];    // the gate's blade (world)
    bool havePose = false, frozen = false;
    // judging (your gates)
    float best = 99, bestSpeed = 0;
    double bestAt = -10;
    bool touched = false;  // a credited contact on the target inside the window
    int verdict = 0;       // 0 pending, 1 clean, 2 perfect, 3 glancing, 4 skipped, 5 blade on blade (dice miss, met), 6 nothing
    double verdictAt = -10;
    bool queued = false;   // its damage was queued (dice hit landed)
    float predErr = -1;    // table vs live hand at the Hit (m)
    bool logged = false;
    bool bodyGate = false;  // placed on the target's chest (the swing was cut short)
};
std::vector<Gate> g_gates;

struct Stats {
    int gates = 0, mine = 0, met = 0, clean = 0, perfect = 0, glancing = 0, skipped = 0, bladeOnBlade = 0;
    int enemy = 0, parries = 0, blocks = 0, hurt = 0, doubleParry = 0, ripostes = 0;
    int stances = 0, featsQueued = 0, featsFired = 0, openings = 0;
    int cueConflicts = 0, chargeShown = 0, chargeLate = 0;
    double timingErrSum = 0;
    int timingN = 0;
    double predErrSum = 0;
    int predN = 0;
    double start = -1, last = -1;
} g_stats;

// one outcome per blow: drives every cue (conflict check: two outcomes on one blow)
struct Outcome {
    DWORD target;
    int kind;
    double at;
};
std::vector<Outcome> g_outcomes;

// world transform of a model (its root part): pos, quat (x, y, z, w)
typedef void(__fastcall* PartGetWorldFn)(void* part, void*, float* pos, float* quat);
int g_quatOrder = 0;  // 0: GetWorld writes (w, x, y, z); 1: (x, y, z, w) - picked by the live-hand check
float g_orderErr[2] = {0, 0};
int g_orderN[2] = {0, 0};

bool ModelWorld(void* gob, float pos[3], float q[4], int order) {
    __try {
        void* root = gob ? *(void**)((BYTE*)gob + 0x6C) : nullptr;
        if (!root) return false;
        float wq[4];
        ((PartGetWorldFn)0x43E4A0)(root, nullptr, pos, wq);
        if (order == 0) q[0] = wq[1], q[1] = wq[2], q[2] = wq[3], q[3] = wq[0];
        else memcpy(q, wq, 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

void ToWorld(const float p[3], const float q[4], const float m[3], float out[3]) {
    QRot(q, m, out);
    for (int k = 0; k < 3; k++) out[k] += p[k];
}

bool CreaturePos(DWORD id, float out[3]) { return Weapon_ObjectPos(id, out); }
DWORD PlayerId() {
    __try {
        BYTE* p = (BYTE*)Weapon_PlayerCreature();
        return p ? *(DWORD*)(p + 4) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

void* GobOf(DWORD id) {
    float p[3];
    return CreaturePos(id, p) ? Anim_ModelAt(p) : nullptr;
}

int RightRow(DWORD id, bool left) {
    typedef void*(__thiscall * GetItemInSlot_t)(void* inventory, DWORD slot);
    __try {
        BYTE* c = (BYTE*)Weapon_CreatureById(id);
        void* inv = c ? *(void**)(c + 0xA2C) : nullptr;
        void* item = inv ? ((GetItemInSlot_t)0x5A4C20)(inv, left ? 0x20 : 0x10) : nullptr;
        return item ? (int)*(DWORD*)((BYTE*)item + 0x0C) : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}

// which attack animation a gate's attacker plays: the model's own (PlayAnimation) if it just started one from the
// table, else the reaction's pair, picked by the attacker's weapon class digit (2 one blade, 3 two-handed, 4 two)
void ResolveAnim(Gate& g) {
    void* gob = GobOf(g.attacker);
    auto it = gob ? g_gobAnim.find(gob) : g_gobAnim.end();
    if (it != g_gobAnim.end() && Now() - it->second.at < 0.4 && it->second.at >= g.rolled - 0.05) {
        const std::string& n = it->second.name;
        if (n.size() == 4 && n[2] == 'a') {
            g.anim = n;
            g.start = it->second.at;
            g.speed = it->second.speed;
        }
    }
    if (g.anim.empty() && g.react >= 0) {
        auto r = g_react.find(g.react);
        if (r != g_react.end() && !r->second.empty()) {
            char want = RightRow(g.attacker, true) >= 0 ? '4' : '2';
            if (g.mine && Weapon_Profile(Weapon_Right()).twoHanded) want = '3';
            g.anim = r->second[0];
            for (auto& c : r->second)
                if (c.size() == 4 && c[1] == want) g.anim = c;
            g.start = g.rolled;
            g.speed = 1;
        }
    }
    auto t = g.anim.empty() ? g_table.end() : g_table.find(g.anim);
    if (t != g_table.end() && t->second.hit > 0) {
        // the engine lands the blow at roll + Hit (measured: damage is queued ~500 ms after the roll for a g2a1 whose
        // animation started 45..210 ms after it), so that is the gate's moment; the pose is the animation's at it
        g.ca = &t->second;
        g.due = g.rolled + t->second.hit / g.speed;
        g.poseT = (float)Clamp((float)((g.due - g.start) * g.speed), 0, t->second.hit);
    }
}

// the gate's blade in the world now (the attacker's model at its Hit pose); enemy gates stand off your chest
// your swing cut short by the engine (the hidden body went back to its ready loop): the gate goes across the
// target's chest, between you - where the cut that lands this blow has to pass
bool BodyGate(Gate& g) {
    float tp[3], me[3];
    if (!CreaturePos(g.target, tp) || !Weapon_PlayerPos(me)) return false;
    float u[3] = {tp[0] - me[0], tp[1] - me[1], 0};
    Norm3(u);
    float side[3] = {u[1], -u[0], 0};
    float c[3] = {tp[0] - u[0] * 0.25f, tp[1] - u[1] * 0.25f, tp[2] + 1.2f};
    for (int k = 0; k < 3; k++) {
        g.a[k] = c[k] - side[k] * 0.28f + (k == 2 ? 0.22f : 0);
        g.b[k] = c[k] + side[k] * 0.28f - (k == 2 ? 0.22f : 0);
    }
    if (!g.bodyGate) Log("fight: your %s was cut short on the model - gate across %08lX's chest", g.anim.c_str(), g.target);
    g.bodyGate = true;
    g.havePose = true;
    return true;
}

bool PlaceGate(Gate& g) {
    if (!g.ca) return false;
    void* gob = GobOf(g.attacker);
    if (g.mine && gob && Now() - g.rolled > 0.15) {
        auto ga = g_gobAnim.find(gob);
        if (ga != g_gobAnim.end() && ga->second.name != g.anim) return BodyGate(g);
    }
    float p[3], q[4], pose[15];
    if (!gob || !ModelWorld(gob, p, q, g_quatOrder) || !ChoreoPose(*g.ca, g.poseT, pose)) return false;
    // the blade's middle stretch (a saber's 0.18..1.16 m): from 20% to 85% of it
    float rb[3], rt[3];
    ToWorld(p, q, pose + 0, rb);
    ToWorld(p, q, pose + 3, rt);
    for (int k = 0; k < 3; k++) {
        g.a[k] = rb[k] + (rt[k] - rb[k]) * 0.15f;
        g.b[k] = rb[k] + (rt[k] - rb[k]) * 0.85f;
    }
    if (g.mine) {
        // your avatar's blade at the blow runs back through your own head and shoulder: the gate is its stretch where
        // it meets the target - 0.6 m of the blade centred on its closest point to the target's chest, at least 0.45 m
        // out from your eye
        float tp[3], eye[3];
        if (CreaturePos(g.target, tp) && EngineCam_EyeWorld(eye)) {
            float lo[3] = {tp[0], tp[1], tp[2] + 0.9f}, hi[3] = {tp[0], tp[1], tp[2] + 1.6f};
            float d[3] = {rt[0] - rb[0], rt[1] - rb[1], rt[2] - rb[2]};
            float len = Len3(d);
            Norm3(d);
            // closest point of the blade to the chest line (sampled)
            float best = 1e9f, bk = 0.5f;
            for (int i = 0; i <= 20; i++) {
                float k = i / 20.0f, p2[3] = {rb[0] + d[0] * len * k, rb[1] + d[1] * len * k, rb[2] + d[2] * len * k};
                float dd = SegSeg(p2, p2, lo, hi);
                if (dd < best) best = dd, bk = k;
            }
            float c[3] = {rb[0] + d[0] * len * bk, rb[1] + d[1] * len * bk, rb[2] + d[2] * len * bk};
            float u[3] = {tp[0] - eye[0], tp[1] - eye[1], 0};
            Norm3(u);
            float out = (c[0] - eye[0]) * u[0] + (c[1] - eye[1]) * u[1];
            if (out < 0.45f) c[0] += u[0] * (0.45f - out), c[1] += u[1] * (0.45f - out);
            for (int k = 0; k < 3; k++) g.a[k] = c[k] - d[k] * 0.3f, g.b[k] = c[k] + d[k] * 0.3f;
        }
    }
    if (!g.mine) {
        // their blade at the blow often ends inside your head: the gate is its 0.6 m stretch around its closest point
        // to your chest line, at least 0.45 m out from your eye toward the attacker - where your blade meets it
        float eye[3], ap[3];
        if (EngineCam_EyeWorld(eye) && CreaturePos(g.attacker, ap)) {
            float lo[3] = {eye[0], eye[1], eye[2] - 0.9f}, hi[3] = {eye[0], eye[1], eye[2] - 0.05f};
            float d[3] = {rt[0] - rb[0], rt[1] - rb[1], rt[2] - rb[2]};
            float len = Len3(d);
            Norm3(d);
            float best = 1e9f, bk = 0.5f;
            for (int i = 0; i <= 20; i++) {
                float k = i / 20.0f, p2[3] = {rb[0] + d[0] * len * k, rb[1] + d[1] * len * k, rb[2] + d[2] * len * k};
                float dd = SegSeg(p2, p2, lo, hi);
                if (dd < best) best = dd, bk = k;
            }
            float c[3] = {rb[0] + d[0] * len * bk, rb[1] + d[1] * len * bk, rb[2] + d[2] * len * bk};
            float u[3] = {ap[0] - eye[0], ap[1] - eye[1], 0};
            Norm3(u);
            float out = (c[0] - eye[0]) * u[0] + (c[1] - eye[1]) * u[1];
            if (out < 0.45f) c[0] += u[0] * (0.45f - out), c[1] += u[1] * (0.45f - out);
            for (int k = 0; k < 3; k++) g.a[k] = c[k] - d[k] * 0.3f, g.b[k] = c[k] + d[k] * 0.3f;
        }
    }
    g.havePose = true;
    return true;
}

// ---------------------------------------------------------------- rounds (charge glow, breath beat)
bool RoundLeft(DWORD id, float* left) {
    __try {
        BYTE* c = (BYTE*)Weapon_CreatureById(id);
        BYTE* round = c ? *(BYTE**)(c + 0x9C8) : nullptr;
        if (!round || !*(round + 0x934)) return false;
        long len = *(long*)(round + 0x94C), timer = *(long*)(round + 0x944);
        if (len <= 0 || len > 10000) return false;
        if (*(int*)(round + 0x958)) return false;  // paused (opened, or waiting on its opponent)
        *left = (len - timer) / 1000.0f;
        return *left >= 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

struct Charge {
    double predicted = -1;  // when its next roll is due
    double shownAt = -1;
    double lastRoll = -10;
};
std::map<DWORD, Charge> g_charge;

// ---------------------------------------------------------------- stances
enum Stance { ST_NONE, ST_NEUTRAL, ST_HIGH, ST_THRUST, ST_LOW, ST_COUNT };
const char* kStanceName[ST_COUNT] = {"none", "neutral", "high", "thrust", "low"};
// the feat each stance asks for, best rank first (feat.2da): Power Attack, Critical Strike, Flurry
const WORD kStanceFeats[ST_COUNT][3] = {{0}, {0}, {83, 17, 28}, {81, 19, 8}, {91, 11, 0}};
const float kStanceRgb[ST_COUNT][3] = {{1, 1, 1}, {0.8f, 0.85f, 1}, {1.0f, 0.55f, 0.15f}, {1.0f, 0.2f, 0.25f}, {0.35f, 0.7f, 1.0f}};
Stance g_stance = ST_NONE, g_stanceCand = ST_NONE;
double g_stanceSince = -10;
bool g_breath;          // the breath beat is on
double g_breathRoll = -1;  // the roll it leads to
WORD g_queuedFeat;      // the feat queued for the coming round (0 none)
double g_queuedAt = -10;
bool g_stanceAvail[ST_COUNT];
WORD g_stanceFeat[ST_COUNT];
double g_stanceFlashAt = -10;

// the ghost blade of a stance, relative to your eye, facing f (horizontal unit), right r: hand and blade direction
void StanceGhost(Stance s, const float eye[3], const float f[3], const float r[3], float hand[3], float dir[3]) {
    float up[3] = {0, 0, 1};
    float h[3], d[3];
    bool dbl = Weapon_Right() == WC_SABER_DOUBLE || Weapon_Right() == WC_BLADE_DOUBLE || Weapon_Right() == WC_STAFF;
    switch (s) {
        case ST_HIGH:  // raised over the shoulder, tip back (staff: overhead, level)
            for (int k = 0; k < 3; k++) h[k] = eye[k] + r[k] * 0.22f + up[k] * 0.08f + f[k] * 0.05f;
            for (int k = 0; k < 3; k++) d[k] = dbl ? r[k] : -f[k] * 0.75f + up[k] * 0.55f + r[k] * 0.1f;
            break;
        case ST_THRUST:  // drawn back at shoulder height, tip at the enemy
            for (int k = 0; k < 3; k++) h[k] = eye[k] + r[k] * 0.2f - up[k] * 0.3f - f[k] * 0.02f;
            for (int k = 0; k < 3; k++) d[k] = f[k];
            break;
        case ST_LOW:  // low at the hip, tip forward (staff: low and level)
            for (int k = 0; k < 3; k++) h[k] = eye[k] + r[k] * 0.2f - up[k] * 0.85f + f[k] * 0.15f;
            for (int k = 0; k < 3; k++) d[k] = dbl ? r[k] : f[k] * 0.85f - up[k] * 0.25f;
            break;
        default:  // neutral: in front, tip up
            for (int k = 0; k < 3; k++) h[k] = eye[k] + r[k] * 0.12f - up[k] * 0.55f + f[k] * 0.32f;
            for (int k = 0; k < 3; k++) d[k] = up[k] * 0.85f + f[k] * 0.3f;
            break;
    }
    Norm3(d);
    memcpy(hand, h, 12);
    memcpy(dir, d, 12);
}

bool Facing(float eye[3], float f[3], float r[3]) {
    if (!EngineCam_EyeWorld(eye)) return false;
    float yaw = EngineCam_ViewYaw() + VR_HeadYaw();
    // the target, when there is one close by, sets the facing (the stance is towards the enemy)
    float pp[3], e[3];
    if (Weapon_PlayerPos(pp) && Melee_NearestEnemy(pp, e)) {
        f[0] = e[0] - eye[0], f[1] = e[1] - eye[1];
    } else {
        f[0] = -sinf(yaw), f[1] = cosf(yaw);  // (unused fallback; fine for drawing)
    }
    f[2] = 0;
    Norm3(f);
    r[0] = f[1], r[1] = -f[0], r[2] = 0;
    return true;
}

// your blade (the right hand's, or the bot's), world: base, tip; false without one
bool g_bladeValid;
float g_bladeBase[3], g_bladeTip[3];
float g_bladeSpeed;

Stance ReadStance() {
    float eye[3], f[3], r[3];
    if (!g_bladeValid || !Facing(eye, f, r)) return ST_NONE;
    float d[3] = {g_bladeTip[0] - g_bladeBase[0], g_bladeTip[1] - g_bladeBase[1], g_bladeTip[2] - g_bladeBase[2]};
    Norm3(d);
    bool dbl = Weapon_Right() == WC_SABER_DOUBLE || Weapon_Right() == WC_BLADE_DOUBLE || Weapon_Right() == WC_STAFF;
    Stance best = ST_NONE;
    float bestScore = 1.6f;
    for (int s = ST_NEUTRAL; s < ST_COUNT; s++) {
        float h[3], gd[3];
        StanceGhost((Stance)s, eye, f, r, h, gd);
        float dh[3] = {g_bladeBase[0] - h[0], g_bladeBase[1] - h[1], g_bladeBase[2] - h[2]};
        float c = Dot3(d, gd);
        if (dbl) c = fabsf(c);
        float score = Len3(dh) / 0.28f + (1 - c) / 0.35f;
        if (score < bestScore) bestScore = score, best = (Stance)s;
    }
    return best;
}

// queue a feat through KotOR's target action menu (actionring.cpp layout): set the column's selected entry, then
// its key 1..3 performs it on the current target. Returns the DIK key to tap, 0 when the menu does not offer it.
BYTE QueueFeat(WORD feat) {
    __try {
        BYTE* app = *(BYTE**)0x7A39FC;
        BYTE* client = app ? *(BYTE**)(app + 4) : nullptr;
        BYTE* inner = client ? *(BYTE**)(client + 4) : nullptr;
        BYTE* gui = inner ? *(BYTE**)(inner + 0x40) : nullptr;
        BYTE* mi = gui ? *(BYTE**)(gui + 0x90) : nullptr;
        BYTE* blk = mi ? mi + 0xBC : nullptr;
        if (!blk) return 0;
        for (int c = 0; c < 3; c++) {
            BYTE* ents = *(BYTE**)(blk + c * 0xC);
            int count = *(int*)(blk + c * 0xC + 4);
            for (int i = 0; ents && i < count && i < 12; i++) {
                int id = *(int*)(ents + i * 0x38 + 8);
                if (id == (int)(0x10000000 | feat)) {
                    int who = *(signed char*)(blk + 0x1AEA);
                    *(int*)(blk + (c + (who + 3) * 3) * 4) = id;
                    static const BYTE kKeys[3] = {0x02, 0x03, 0x04};
                    return kKeys[c];
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

bool CanUse(WORD feat) {
    typedef bool(__thiscall * CanUseFeat_t)(void*, unsigned short);
    __try {
        BYTE* p = (BYTE*)Weapon_PlayerCreature();
        void* stats = p ? *(void**)(p + 0xA74) : nullptr;
        return stats && feat && ((CanUseFeat_t)0x5A6680)(stats, feat);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

BYTE g_tapKey;  // a key for input.cpp to tap (the feat's column)

// ---------------------------------------------------------------- enemy health (the status band)
std::map<DWORD, short> g_maxHp;
bool HpOf(DWORD id, short* hp, short* maxHp) {
    __try {
        BYTE* c = (BYTE*)Weapon_CreatureById(id);
        if (!c) return false;
        *hp = *(short*)(c + 0xDC);
        short& m = g_maxHp[id];
        if (*hp > m) m = *hp;
        *maxHp = m;
        return m > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// ---------------------------------------------------------------- outcomes
enum OutcomeKind { OC_CLEAN = 1, OC_PERFECT, OC_GLANCING, OC_SKIPPED, OC_BLADE_ON_BLADE, OC_NOTHING, OC_PARRIED,
                   OC_BLOCKED, OC_HURT, OC_OPENING, OC_RIPOSTE };
const char* kOutcome[] = {"", "clean", "perfect", "glancing", "skipped", "blade on blade", "nothing", "parried",
                          "blocked", "hurt", "opening", "riposte"};
struct Flash {
    float pos[3];
    float rgb[3];
    double at;
    float size;
};
std::vector<Flash> g_flashes;

void FlashAt(const float pos[3], float r, float g, float b, float size) {
    Flash f;
    memcpy(f.pos, pos, 12);
    f.rgb[0] = r, f.rgb[1] = g, f.rgb[2] = b;
    f.at = Now();
    f.size = size;
    g_flashes.push_back(f);
    if (g_flashes.size() > 16) g_flashes.erase(g_flashes.begin());
}

void FireOutcome(DWORD target, int kind, const float* at) {
    double t = Now();
    for (const Outcome& o : g_outcomes)
        if (o.target == target && t - o.at < 0.12 && o.kind != kind && kind != OC_OPENING && o.kind != OC_OPENING) {
            g_stats.cueConflicts++;
            Log("fight: CUE CONFLICT on %08lX: %s then %s within 120 ms", target, kOutcome[o.kind], kOutcome[kind]);
        }
    g_outcomes.push_back({target, kind, t});
    if (g_outcomes.size() > 32) g_outcomes.erase(g_outcomes.begin());
    float p[3] = {0, 0, 0};
    if (at) memcpy(p, at, 12);
    else if (CreaturePos(target, p)) p[2] += 1.2f;
    switch (kind) {
        case OC_PERFECT:  // the enemy's own flinch, a long freeze, a white-gold burst, a double crack
            Anim_HitPause(target, 0.11f);
            Cues_Fire(CUE_PERFECT_LANDED);
            FlashAt(p, 1.0f, 0.9f, 0.6f, 0.45f);
            break;
        case OC_CLEAN:
            Anim_HitPause(target, 0.07f);
            Cues_Fire(CUE_HIT_LANDED);
            FlashAt(p, 1.0f, 0.55f, 0.15f, 0.32f);
            break;
        case OC_GLANCING:  // no freeze: a scrape, a dull spark
            Cues_Glance();
            Cues_Tone("cb_ls_swingshrt1", 0.35f, 0.7f);
            FlashAt(p, 0.6f, 0.6f, 0.6f, 0.18f);
            break;
        case OC_BLADE_ON_BLADE:  // its parry met your blade: clash, blue sparks, your blade knocked back
            Cues_Fire(CUE_BLOCK);
            FlashAt(p, 0.55f, 0.8f, 1.0f, 0.3f);
            break;
        case OC_PARRIED:
            FlashAt(p, 0.9f, 0.95f, 1.0f, 0.4f);
            break;  // (combat.cpp fired the parry's own cue, stagger and hit-pause)
        case OC_BLOCKED:
            FlashAt(p, 0.5f, 0.75f, 1.0f, 0.25f);
            break;
        case OC_OPENING:
            Cues_Fire(CUE_PERFECT_LANDED);
            FlashAt(p, 1.0f, 0.8f, 0.3f, 0.55f);
            break;
        case OC_RIPOSTE:
            Anim_HitPause(target, 0.12f);
            Cues_Fire(CUE_PERFECT_LANDED);
            FlashAt(p, 1.0f, 0.95f, 0.8f, 0.5f);
            break;
        default:
            break;
    }
}

void NoteFight() {
    double t = Now();
    if (g_stats.start < 0) {
        g_stats.start = t;
        Log("fight: begins");
    }
    g_stats.last = t;
}

// ---------------------------------------------------------------- riposte
struct Riposte {
    DWORD target;
    float a[3], b[3];
    double due;
    float best = 99;
    double bestAt = -10;
    bool done;
};
std::vector<Riposte> g_ripostes;

// ---------------------------------------------------------------- the bot ([Debug] Bot / simcmd "bot N")
struct BotPlan {
    bool active = false;
    float c[3], s[3], u[3];  // sweep centre, sweep direction, blade axis
    double t0, t1;           // sweep from .. to
    bool hold = false;       // a held block (no sweep)
    DWORD gateAttacker = 0;  // the gate it aims at (followed every frame: gates move with their model until frozen)
    double gateDue = -1;
    float jitter[3] = {0, 0, 0};
};
BotPlan g_botPlan;
double g_botNextOpen = 0;
unsigned g_botSeed = 12345;
float BotRand() {
    g_botSeed = g_botSeed * 1103515245u + 12345u;
    return ((g_botSeed >> 8) & 0xFFFF) / 65535.0f;
}
Stance g_botStance = ST_NONE;
int g_botStanceTurn;

void BotSweep(const float c[3], const float along[3], double due, double lead) {
    float s[3], u[3];
    // sweep across the gate's blade: s = horizontal-ish, perpendicular to it; blade axis u = along x s (upright-ish)
    float up[3] = {0, 0, 1};
    s[0] = along[1] * up[2] - along[2] * up[1], s[1] = along[2] * up[0] - along[0] * up[2], s[2] = along[0] * up[1] - along[1] * up[0];
    if (Len3(s) < 0.2f) s[0] = 1, s[1] = 0, s[2] = 0;
    Norm3(s);
    u[0] = along[1] * s[2] - along[2] * s[1], u[1] = along[2] * s[0] - along[0] * s[2], u[2] = along[0] * s[1] - along[1] * s[0];
    Norm3(u);
    if (u[2] < 0) u[0] = -u[0], u[1] = -u[1], u[2] = -u[2];
    memcpy(g_botPlan.c, c, 12);
    memcpy(g_botPlan.s, s, 12);
    memcpy(g_botPlan.u, u, 12);
    g_botPlan.t0 = due - lead;
    g_botPlan.t1 = due + 0.08;
    g_botPlan.hold = false;
    g_botPlan.active = true;
    g_botPlan.gateDue = -1;
}

// a blow with no choreography (monsters): the block bar - swung onto it 0.15 s before the impact (a parry)
bool BotBar(double t) {
    GuardBar bars[8];
    int n = Guard_Bars(bars, 8);
    const GuardBar* best = nullptr;
    for (int i = 0; i < n; i++)
        if (!bars[i].result && bars[i].secondsLeft > 0.08f && bars[i].secondsLeft < 0.7f && (!best || bars[i].secondsLeft < best->secondsLeft))
            best = &bars[i];
    if (!best) return false;
    float c[3], along[3];
    for (int k = 0; k < 3; k++) c[k] = (best->a[k] + best->b[k]) * 0.5f, along[k] = best->b[k] - best->a[k];
    Norm3(along);
    double due = t + best->secondsLeft - 0.12;
    if (g_bot == 1) {
        if (BotRand() < 0.45f) return true;  // sloppy: lets it through
        due += (BotRand() - 0.5f) * 0.6f;
    }
    // lay the blade along the bar (block) arriving with speed (parry): sweep along its normal into it, end on it
    float up[3] = {0, 0, 1}, s[3];
    s[0] = along[1] * up[2] - along[2] * up[1], s[1] = along[2] * up[0] - along[0] * up[2], s[2] = along[0] * up[1] - along[1] * up[0];
    if (Len3(s) < 0.2f) s[0] = 0, s[1] = 0, s[2] = 1;
    Norm3(s);
    memcpy(g_botPlan.c, c, 12);
    memcpy(g_botPlan.s, s, 12);
    memcpy(g_botPlan.u, along, 12);
    g_botPlan.t0 = due - 0.15;
    g_botPlan.t1 = due + 0.25;
    g_botPlan.hold = true;
    g_botPlan.active = true;
    return true;
}

void BotUpdate() {
    if (!g_bot) return;
    double t = Now();
    float eye[3], f[3], r[3];
    if (!Facing(eye, f, r)) return;
    float pp[3], e[3];
    DWORD enemy = Weapon_PlayerPos(pp) ? Melee_NearestEnemy(pp, e) : 0;
    bool perfect = g_bot == 2, sloppy = g_bot == 1, idle = g_bot == 3;
    // plan: the next gate due (an enemy's to parry, or yours to cut through)
    if (!idle && (!g_botPlan.active || t > g_botPlan.t1)) {
        g_botPlan.active = false;
        const Gate* next = nullptr;
        for (const Gate& g : g_gates)
            if (g.havePose && g.due > t + 0.05 && g.due < t + 0.9 && g.verdict == 0 && (!next || g.due < next->due)) next = &g;
        if (next) {
            float c[3], along[3];
            for (int k = 0; k < 3; k++) c[k] = (next->a[k] + next->b[k]) * 0.5f, along[k] = next->b[k] - next->a[k];
            Norm3(along);
            double due = next->due;
            bool skip = false;
            if (sloppy) {  // half its gates, the rest off by up to 0.45 s and 0.3 m
                skip = BotRand() < 0.45f;
                due += (BotRand() - 0.5f) * 0.9f;
                for (int k = 0; k < 3; k++) c[k] += (BotRand() - 0.5f) * 0.6f;
            }
            if (!skip) {
                BotSweep(c, along, due, next->mine ? 0.12 : 0.1);
                g_botPlan.gateAttacker = next->attacker;
                g_botPlan.gateDue = next->due;
                float mid[3];
                for (int k = 0; k < 3; k++) mid[k] = (next->a[k] + next->b[k]) * 0.5f, g_botPlan.jitter[k] = c[k] - mid[k];
            }
        } else if (!idle && BotBar(t)) {
            // (a monster's blow: no choreography, the block bar - planned in BotBar)
        } else if (enemy && t > g_botNextOpen && !Combat_PlayerAttacking() &&
                   (e[0] - pp[0]) * (e[0] - pp[0]) + (e[1] - pp[1]) * (e[1] - pp[1]) < 1.9f * 1.9f) {
            // nothing due and no fight going: a cut through its chest starts the fight (the contact rule)
            float c[3] = {e[0], e[1], e[2] + 1.2f}, along[3] = {0, 0, 1};
            BotSweep(c, along, t + 0.25, 0.25);
            g_botNextOpen = t + 1.5;
        } else if (enemy && t > g_botNextOpen && !Combat_PlayerAttacking() && !SimCmd_Driving()) {
            // out of reach: close in (the harness autopilot, as the stick would)
            SimCmd_Do("approach 1.1");
            g_botNextOpen = t + 2.5;
        }
    }
    if (g_botPlan.active && g_botPlan.gateDue > 0)  // follow the gate: re-aim at where it is now
        for (const Gate& g : g_gates)
            if (g.attacker == g_botPlan.gateAttacker && fabs(g.due - g_botPlan.gateDue) < 0.01 && g.havePose)
                for (int k = 0; k < 3; k++) g_botPlan.c[k] = (g.a[k] + g.b[k]) * 0.5f + g_botPlan.jitter[k];
    float base[3], tip[3];
    if (g_botPlan.active && t >= g_botPlan.t0 - 0.25 && t <= g_botPlan.t1 + 0.1) {
        // wind up 0.4 m before the centre, sweep 0.8 m through it between t0 and t1 (~3.5 m/s), mid-blade on it
        float k = (float)Clamp((float)((t - g_botPlan.t0) / (g_botPlan.t1 - g_botPlan.t0)), 0, 1);
        if (g_botPlan.hold) {  // into the bar over its first 40%, then held on it
            float x = -0.35f + 0.35f * Clamp(k / 0.4f, 0, 1);
            for (int i = 0; i < 3; i++) {
                base[i] = g_botPlan.c[i] + g_botPlan.s[i] * x - g_botPlan.u[i] * 0.4f;
                tip[i] = base[i] + g_botPlan.u[i] * 1.0f;
            }
        } else {
            float x = -0.4f + 0.8f * k;
            for (int i = 0; i < 3; i++) {
                base[i] = g_botPlan.c[i] + g_botPlan.s[i] * x - g_botPlan.u[i] * 0.55f;
                tip[i] = base[i] + g_botPlan.u[i] * 1.0f;
            }
        }
    } else {
        // between gates: the stance for the coming round (cycling high / thrust / low / neutral), else neutral guard
        Stance s = ST_NEUTRAL;
        if (g_breath && !idle) s = g_botStance;
        float h[3], d[3];
        StanceGhost(s, eye, f, r, h, d);
        for (int i = 0; i < 3; i++) base[i] = h[i], tip[i] = h[i] + d[i] * 1.0f;
    }
    Melee_BotBlade(1, base, tip);
}

// ---------------------------------------------------------------- GL (resolved lazily; the warnings pass has a context)
typedef void(APIENTRY* glBegin_t)(GLenum);
typedef void(APIENTRY* glEnd_t)();
typedef void(APIENTRY* glVertex3f_t)(GLfloat, GLfloat, GLfloat);
typedef void(APIENTRY* glColor4f_t)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void(APIENTRY* glLineWidth_t)(GLfloat);
glBegin_t gBegin;
glEnd_t gEnd;
glVertex3f_t gV;
glColor4f_t gC;
glLineWidth_t gLW;
bool GlReady() {
    if (gBegin) return true;
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) return false;
    gBegin = (glBegin_t)GetProcAddress(gl, "glBegin");
    gEnd = (glEnd_t)GetProcAddress(gl, "glEnd");
    gV = (glVertex3f_t)GetProcAddress(gl, "glVertex3f");
    gC = (glColor4f_t)GetProcAddress(gl, "glColor4f");
    gLW = (glLineWidth_t)GetProcAddress(gl, "glLineWidth");
    return gBegin && gEnd && gV && gC;
}

void ToEye(const float wv[16], const float w[3], float e[3]) {
    for (int r = 0; r < 3; r++) e[r] = wv[0 * 4 + r] * w[0] + wv[1 * 4 + r] * w[1] + wv[2 * 4 + r] * w[2] + wv[12 + r];
}

// a camera-facing ribbon between two eye-space points
void Ribbon(const float a[3], const float b[3], float thick, float r, float g, float bl, float alpha) {
    float sx = b[0] - a[0], sy = b[1] - a[1], sl = sqrtf(sx * sx + sy * sy) + 1e-5f;
    float nx = -sy / sl * thick, ny = sx / sl * thick;
    gC(r, g, bl, alpha);
    gBegin(GL_TRIANGLES);
    gV(a[0] - nx, a[1] - ny, a[2]);
    gV(b[0] - nx, b[1] - ny, b[2]);
    gV(b[0] + nx, b[1] + ny, b[2]);
    gV(a[0] - nx, a[1] - ny, a[2]);
    gV(b[0] + nx, b[1] + ny, b[2]);
    gV(a[0] + nx, a[1] + ny, a[2]);
    gEnd();
}

void Ring(const float c[3], float rad, float r, float g, float b, float a, int seg = 32, float from = 0, float to = 1) {
    gC(r, g, b, a);
    gBegin(GL_LINE_STRIP);
    for (int k = 0; k <= seg; k++) {
        float an = (from + (to - from) * k / seg) * 6.2831853f;
        gV(c[0] + rad * cosf(an), c[1] + rad * sinf(an), c[2]);
    }
    gEnd();
}

void Glyph(Stance s, const float c[3], float sz, const float rgb[3], float a) {
    gC(rgb[0], rgb[1], rgb[2], a);
    gBegin(GL_TRIANGLES);
    if (s == ST_HIGH) {  // Power Attack: a heavy downward wedge
        gV(c[0] - sz, c[1] + sz * 0.6f, c[2]);
        gV(c[0] + sz, c[1] + sz * 0.6f, c[2]);
        gV(c[0], c[1] - sz, c[2]);
    } else if (s == ST_THRUST) {  // Critical Strike: a point (diamond)
        gV(c[0], c[1] + sz, c[2]);
        gV(c[0] - sz * 0.5f, c[1], c[2]);
        gV(c[0] + sz * 0.5f, c[1], c[2]);
        gV(c[0], c[1] - sz, c[2]);
        gV(c[0] + sz * 0.5f, c[1], c[2]);
        gV(c[0] - sz * 0.5f, c[1], c[2]);
    } else if (s == ST_LOW) {  // Flurry: three quick strokes
        for (int i = -1; i <= 1; i++) {
            float x = c[0] + i * sz * 0.7f;
            gV(x - sz * 0.15f, c[1] - sz, c[2]);
            gV(x + sz * 0.35f, c[1] + sz, c[2]);
            gV(x + sz * 0.05f, c[1] - sz, c[2]);
        }
    }
    gEnd();
}

}  // namespace

// ================================================================ API
void Fight_Init(const char* gameDir) {
    const char* ini = Features_IniPath();
    char mode[32], v[32];
    GetPrivateProfileStringA("Combat", "Mode", "choreo", mode, sizeof mode, ini);
    g_active = _stricmp(mode, "choreo") == 0;
    GetPrivateProfileStringA("Combat", "GateWindow", "0.35", v, sizeof v, ini);
    g_window = Clamp((float)atof(v), 0.1f, 1.0f);
    GetPrivateProfileStringA("Combat", "GateRadius", "0.22", v, sizeof v, ini);
    g_radius = Clamp((float)atof(v), 0.08f, 0.6f);
    GetPrivateProfileStringA("Combat", "IncomingDamage", "1.0", v, sizeof v, ini);
    g_incoming = Clamp((float)atof(v), 0.0f, 4.0f);
    g_ghosts = GetPrivateProfileIntA("Combat", "StanceGhosts", 1, ini) != 0;
    g_grade = GetPrivateProfileIntA("Combat", "FightGrade", 1, ini) != 0;
    g_bot = GetPrivateProfileIntA("Debug", "Bot", 0, ini);
    g_animLog = GetPrivateProfileIntA("Debug", "AnimLog", 0, ini) != 0;
    sprintf_s(g_statsIni, "%s\\kvr\\fight_stats.ini", gameDir);
    g_fightsSeen = GetPrivateProfileIntA("Fights", "Count", 0, g_statsIni);
    LoadTable(gameDir);
    if (Combat_Detour(kPlayVA, kPlayOrig, 6, PlayAnimStub, &g_playTramp, "model PlayAnimation"))
        Log("fight: model animations watched");
    Log("fight: mode %s, gate window %.2f s, radius %.2f m, incoming damage x%.2f, bot %d, %d fights so far",
        g_active ? "choreo" : mode, g_window, g_radius, g_incoming, g_bot, g_fightsSeen);
}

bool Fight_Active() { return g_active; }
float Fight_IncomingDamage() { return g_active ? g_incoming : 1.0f; }
void Fight_SetBot(int mode) {
    g_bot = mode;
    g_botPlan.active = false;
    Melee_BotRelease(mode == 0);
    Log("fight: bot %s", mode == 2 ? "perfect" : mode == 1 ? "sloppy" : mode == 3 ? "idle" : "off");
}
int Fight_Bot() { return g_bot; }

void Fight_NoteRoll(DWORD attacker, DWORD target, BYTE* record, bool playerAttacks, bool targetIsPlayer) {
    if (!g_active || !record || !(playerAttacks || targetIsPlayer)) return;
    Gate g;
    g.attacker = attacker;
    g.target = target;
    g.mine = playerAttacks;
    g.record = record;
    g.rolled = Now();
    g_gates.push_back(g);
    if (g_gates.size() > 24) g_gates.erase(g_gates.begin());
    if (targetIsPlayer) {  // charge prediction check: how early the glow came for this roll
        Charge& c = g_charge[attacker];
        if (c.predicted > 0) {
            double err = g.rolled - c.predicted;
            if (fabs(err) < 1.0) {
                if (c.shownAt > 0 && g.rolled - c.shownAt >= 0.5) g_stats.chargeShown++;
                else g_stats.chargeLate++;
            }
            static int logged;
            if (logged++ < 40) Log("fight: charge for %08lX predicted the roll %+.0f ms off (glow %.2f s ahead)", attacker,
                                   err * 1000, c.shownAt > 0 ? g.rolled - c.shownAt : -1.0);
        }
        c.lastRoll = g.rolled;
        c.predicted = -1;
        c.shownAt = -1;
    }
    // (whether a queued feat was taken is read after the roll, in Fight_Update: the resolver writes it)
    NoteFight();
}

bool Fight_EnemyGate(DWORD attacker, float a[3], float b[3], float* secondsLeft, float* total) {
    if (!g_active) return false;
    double t = Now();
    for (const Gate& g : g_gates) {
        if (g.mine || g.attacker != attacker || !g.havePose || g.due < 0) continue;
        if (t > g.due + 0.2) continue;
        memcpy(a, g.a, 12);
        memcpy(b, g.b, 12);
        *secondsLeft = (float)(g.due - t);
        *total = (float)(g.due - g.rolled);
        return true;
    }
    return false;
}

DWORD Fight_OpenEnemy(float pos[3]) {
    float pp[3], e[3];
    DWORD id = Weapon_PlayerPos(pp) ? Melee_NearestEnemy(pp, e) : 0;
    if (!id) return 0;
    float v, m, left;
    if (Combat_PostureInfo(id, &v, &m, &left) && left > 0) {
        memcpy(pos, e, 12);
        return id;
    }
    return 0;
}

bool Fight_OnBeat() {
    if (!g_active) return false;
    double t = Now();
    if (g_breath) return true;
    for (const Gate& g : g_gates)
        if (g.due > 0 && fabs(t - g.due) < 0.15) return true;
    return false;
}

// your blow lands: its gate decides how
static void PlayNow(void* gob, const char* name) {
    typedef void(__thiscall * PlayFn)(void* gob, const char* name, float speed, int flags, float start);
    __try {
        ((PlayFn)g_playTramp)(gob, name, 1.0f, 0, 0.0f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool Fight_PlayerBlowLands(DWORD target, int* ints) {
    if (!g_active) return false;
    double t = Now();
    Gate* gate = nullptr;
    for (Gate& g : g_gates)
        if (g.mine && g.target == target && !g.queued && t - g.rolled < 3.5 && (!gate || g.rolled < gate->rolled)) gate = &g;
    int dealt = 0;
    for (int i = 0; i < 15; i++) dealt += ints[i] > 0 ? ints[i] : 0;
    if (!gate) {
        Log("fight: your blow on %08lX has no gate - contact rules", target);
        return false;
    }
    gate->queued = true;
    // calibration: the table's Hit against the engine's (when the damage really came)
    if (gate->due > 0) {
        double off = t - gate->due;
        static double sum;
        static int n;
        if (fabs(off) < 0.5) sum += off, n++;
        static int logged;
        if (logged++ < 30) Log("fight: hit timing: engine %+.0f ms vs the table (mean %+.0f over %d)", off * 1000, n ? sum / n * 1000 : 0, n);
    }
    bool credit = Melee_HasCredit(target);
    bool met = gate->best <= g_radius + g_abBonus && gate->bestSpeed >= 1.2f;
    double err = gate->bestAt - (gate->due > 0 ? gate->due : t);
    bool onTime = met && err >= -g_window && err <= 0.12;
    bool perfect = onTime && fabs(err) <= 0.12;
    int kind;
    float k;
    if (onTime || (credit && !gate->havePose)) {  // (no table pose: a contact in time counts as the gate)
        kind = perfect ? OC_PERFECT : OC_CLEAN;
        k = perfect ? 1.15f : 1.0f;
    } else if (met || credit || gate->touched) {
        kind = OC_GLANCING;
        k = 0.5f;
    } else {
        kind = OC_SKIPPED;
        k = 0;
        if (void* gob = GobOf(target)) {  // never swung: no hit, and its flinch becomes a parry
            g_swapUntil[gob] = t + 0.25;
            auto ga = g_gobAnim.find(gob);
            if (ga != g_gobAnim.end() && t - ga->second.at < 1.5) {
                if (const char* p = ParryFor(ga->second.name)) PlayNow(gob, p);
            }
        }
    }
    for (int i = 0; i < 15; i++)
        if (ints[i] > 0) ints[i] = (int)(ints[i] * k + 0.5f);
    if (kind != OC_SKIPPED) {
        Combat_HitPosture(target, kind == OC_PERFECT ? 22.0f : kind == OC_CLEAN ? 12.0f : 4.0f, "your blow");
        Melee_TakeCredit(target, nullptr, nullptr);
        float hitAt[3];
        for (int i = 0; i < 3; i++) hitAt[i] = gate->havePose ? (gate->a[i] + gate->b[i]) * 0.5f : 0;
        FireOutcome(target, kind, gate->havePose ? hitAt : nullptr);
    }
    gate->verdict = kind == OC_PERFECT ? 2 : kind == OC_CLEAN ? 1 : kind == OC_GLANCING ? 3 : 4;
    gate->verdictAt = t;
    g_stats.mine++;
    if (met) g_stats.met++;
    if (kind == OC_PERFECT) g_stats.perfect++, g_stats.clean++;
    else if (kind == OC_CLEAN) g_stats.clean++;
    else if (kind == OC_GLANCING) g_stats.glancing++;
    else g_stats.skipped++;
    if (met) g_stats.timingErrSum += fabs(err), g_stats.timingN++;
    Log("gate: yours on %08lX (%s, dice hit) -> %s: %d of %d damage, blade %.2f m from it at %+.0f ms (%.1f m/s)%s",
        target, gate->anim.empty() ? "?" : gate->anim.c_str(), kOutcome[kind], (int)(dealt * k + 0.5f), dealt,
        gate->best, err * 1000, gate->bestSpeed, gate->havePose ? "" : " [no pose]");
    return true;
}

void Fight_DefenceResult(DWORD attacker, int result, int damage) {
    if (!g_active) return;
    double t = Now();
    Gate* gate = nullptr;
    for (Gate& g : g_gates)
        if (!g.mine && g.attacker == attacker && g.verdict == 0 && (!gate || g.rolled < gate->rolled)) gate = &g;
    g_stats.enemy++;
    static double lastParry = -10;
    if (result == 2) {
        g_stats.parries++;
        if (t - lastParry < 0.3) {
            g_stats.doubleParry++;
            Log("fight: double parry");
        }
        lastParry = t;
        FireOutcome(attacker, OC_PARRIED, gate && gate->havePose ? gate->a : nullptr);
        // a perfect parry (within 0.15 s of the Hit) opens a riposte gate on the next half-beat
        if (gate && gate->due > 0 && fabs(t - gate->due) < 0.2) {
            float ap[3];
            if (CreaturePos(attacker, ap)) {
                Riposte r;
                r.target = attacker;
                float eye[3];
                EngineCam_EyeWorld(eye);
                float u[3] = {ap[0] - eye[0], ap[1] - eye[1], 0};
                Norm3(u);
                float side[3] = {u[1], -u[0], 0};
                float c[3] = {ap[0] - u[0] * 0.25f, ap[1] - u[1] * 0.25f, ap[2] + 1.15f};
                for (int k = 0; k < 3; k++) r.a[k] = c[k] - side[k] * 0.3f + (k == 2 ? 0.25f : 0), r.b[k] = c[k] + side[k] * 0.3f - (k == 2 ? 0.25f : 0);
                r.due = t + 0.375;
                r.done = false;
                g_ripostes.push_back(r);
                Log("fight: riposte gate on %08lX (0.375 s)", attacker);
            }
        }
    } else if (result == 1) {
        g_stats.blocks++;
        FireOutcome(attacker, OC_BLOCKED, gate && gate->havePose ? gate->a : nullptr);
    } else if (damage > 0) {
        g_stats.hurt++;
    }
    if (gate) {
        gate->verdict = result == 2 ? 7 : result == 1 ? 8 : 9;
        gate->verdictAt = t;
        Log("gate: %08lX's on you (%s) -> %s, %d damage", attacker, gate->anim.empty() ? "?" : gate->anim.c_str(),
            result == 2 ? "parried" : result == 1 ? "blocked" : "hit you", damage);
    }
}

void Fight_Summary(const char* why) {
    if (g_stats.start < 0) return;
    double meanErr = g_stats.timingN ? g_stats.timingErrSum / g_stats.timingN * 1000 : 0;
    double pred = g_stats.predN ? g_stats.predErrSum / g_stats.predN : -1;
    Log("fight: summary (%s) %.0f s | your gates %d: met %d, clean %d (perfect %d), glancing %d, skipped %d, blade-on-blade %d"
        " | theirs %d: parried %d, blocked %d, hurt %d, double parries %d, ripostes %d | stances %d, feats queued %d fired %d"
        " | openings %d | charge ahead %d late %d | cue conflicts %d | timing err %.0f ms | gate pose err %.2f m (order %d)",
        why, g_stats.last - g_stats.start, g_stats.mine, g_stats.met, g_stats.clean, g_stats.perfect, g_stats.glancing,
        g_stats.skipped, g_stats.bladeOnBlade, g_stats.enemy, g_stats.parries, g_stats.blocks, g_stats.hurt,
        g_stats.doubleParry, g_stats.ripostes, g_stats.stances, g_stats.featsQueued, g_stats.featsFired, g_stats.openings,
        g_stats.chargeShown, g_stats.chargeLate, g_stats.cueConflicts, meanErr, pred, g_quatOrder);
    if (g_grade && g_stats.enemy + g_stats.mine >= 3) {  // the exchange grade, in the world for 4 s
        char text[160];
        sprintf_s(text, "#Fight\nParried %d of %d\nGates %d of %d\nHits taken %d", g_stats.parries, g_stats.enemy,
                  g_stats.clean + g_stats.bladeOnBlade, g_stats.mine, g_stats.hurt);
        float eye[3], f[3], r[3];
        if (Facing(eye, f, r)) {
            float w[3] = {eye[0] + f[0] * 1.4f, eye[1] + f[1] * 1.4f, eye[2] - 0.15f}, pos[3];
            if (EngineCam_WorldToTracking(w, pos)) {
                float head[3];
                VR_HeadPos(head);
                float yaw = atan2f(head[0] - pos[0], head[2] - pos[2]);
                float q[4] = {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
                Hover_Suppress(4.0);
                VR_ShowText(3, KVR_ANCHOR_WORLD, pos, q, 0.32f, text, -1);
                g_gradeUntil = Now() + 4.0;
            }
        }
    }
    g_fightsSeen++;
    char n[16];
    sprintf_s(n, "%d", g_fightsSeen);
    WritePrivateProfileStringA("Fights", "Count", n, g_statsIni);
    g_stats = Stats();
}

void Fight_Update() {
    if (!g_active) return;
    double t = Now();
    // your blade this frame (melee.cpp keeps it, the bot may drive it)
    g_bladeValid = Melee_BladeNow(1, g_bladeBase, g_bladeTip, &g_bladeSpeed);
    BotUpdate();
    if (g_bot) g_bladeValid = Melee_BladeNow(1, g_bladeBase, g_bladeTip, &g_bladeSpeed);

    // gates: read the record once it is written, place it, judge yours
    for (Gate& g : g_gates) {
        if (!g.read && t - g.rolled > 0.02) {
            g.read = true;
            __try {
                g.dice = *(int*)(g.record + 0x5C);
                g.react = *(short*)(g.record + 0x12);
                g.feat = *(WORD*)(g.record + 0x64);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            // progression felt as windows (S12): your attack bonus widens your gates, your Defense the parry window
            {
                int ab = 0, def = 0;
                __try {
                    ab = *(signed char*)(g.record + 0xC2);
                    def = *(signed char*)(g.record + 0xF4);
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                if (g.mine && ab > -5 && ab < 60) g_abBonus = Clamp((ab - 5) * 0.006f, 0, 0.09f);
                if (!g.mine && def > 0 && def < 80) Guard_SetParryBonus((def - 15) * 0.006f);
                if (g.mine) Log("fight: your roll: attack type %u, dice %d, attack bonus %d", g.feat, g.dice, ab);
                else {
                    static int dl;
                    if (dl++ < 10) Log("fight: their roll on you: dice %d, your defense %d", g.dice, def);
                }
            }
            if (g.mine && g_queuedFeat && t - g_queuedAt < 7.0) {  // KotOR may take it this round or the next
                bool took = g.feat == g_queuedFeat;
                if (took) g_stats.featsFired++;
                Log("fight: feat %u %s by this roll (attack type %u)", g_queuedFeat, took ? "taken" : "not yet taken", g.feat);
                if (took) g_queuedFeat = 0;
            } else if (g.mine && g_queuedFeat) {
                Log("fight: feat %u never taken", g_queuedFeat);
                g_queuedFeat = 0;
            }
        }
        if (g.read && g.due < 0 && t - g.rolled < 0.6) ResolveAnim(g);
        if (g.read && g.ca && !g.frozen) {
            PlaceGate(g);
            if (g.due - t < 0.25) g.frozen = true;
        }
        if (g.read && g.due > 0 && g.ca && !g.logged) {
            g.logged = true;
            static int logged;
            if (logged++ < 200)
                Log("gate: %s %08lX -> %08lX, %s, dice %d, react %d (%s), Hit in %.0f ms%s", g.mine ? "yours" : "theirs",
                    g.attacker, g.target, g.anim.c_str(), g.dice, g.react, g_rowName.count(g.react) ? g_rowName[g.react].c_str() : "?",
                    (g.due - g.rolled) * 1000, g.havePose ? "" : " (no pose)");
        }
        // the live hand at the Hit against the table (both quaternion orders): calibrates the transform
        if (g.ca && g.due > 0 && g.predErr < 0 && t >= g.due) {
            void* gob = GobOf(g.attacker);
            float live[3], pose[15];
            if (gob && !g.bodyGate && Anim_PartPos(gob, "rhand", live) && ChoreoPose(*g.ca, g.poseT, pose)) {
                for (int o = 0; o < 2; o++) {
                    float p[3], q[4], w[3];
                    if (!ModelWorld(gob, p, q, o)) continue;
                    // the table's rhand: the blade base minus its 0.18 m along the hook's +z
                    float rb[3] = {pose[0], pose[1], pose[2]}, rt[3] = {pose[3], pose[4], pose[5]}, d[3];
                    for (int k = 0; k < 3; k++) d[k] = rt[k] - rb[k];
                    Norm3(d);
                    float hand[3] = {rb[0] - d[0] * 0.18f, rb[1] - d[1] * 0.18f, rb[2] - d[2] * 0.18f};
                    ToWorld(p, q, hand, w);
                    float e[3] = {w[0] - live[0], w[1] - live[1], w[2] - live[2]};
                    float err = Len3(e);
                    g_orderErr[o] += err;
                    g_orderN[o]++;
                    if (o == g_quatOrder) g.predErr = err, g_stats.predErrSum += err, g_stats.predN++;
                }
                if (g_orderN[0] >= 4 && g_orderN[1] >= 4) {
                    int best = g_orderErr[1] / g_orderN[1] < g_orderErr[0] / g_orderN[0] ? 1 : 0;
                    if (best != g_quatOrder) {
                        Log("fight: model quaternion order -> %d (mean hand error %.2f vs %.2f m)", best,
                            g_orderErr[best] / g_orderN[best], g_orderErr[g_quatOrder] / g_orderN[g_quatOrder]);
                        g_quatOrder = best;
                    }
                }
                static int logged;
                auto ga = g_gobAnim.find(gob);
                if (logged++ < 60)
                    Log("fight: %s pose check at the blow: hand %.2f m from the table (%s at %.2f s, dice %d; the model plays %s, %.2f s in)",
                        g.mine ? "your" : "their", g.predErr, g.anim.c_str(), g.poseT, g.dice,
                        ga != g_gobAnim.end() ? ga->second.name.c_str() : "?", ga != g_gobAnim.end() ? t - ga->second.at : -1.0);
            }
        }
        if (g.mine && g.havePose && g.verdict == 0 && g_bladeValid && g.due > 0 && t >= g.due - g_window - 0.1 && t <= g.due + 0.15) {
            float d = SegSeg(g_bladeBase, g_bladeTip, g.a, g.b);
            if (d < g.best) {
                g.best = d;
                g.bestAt = t;
                g.bestSpeed = g_bladeSpeed;
            } else if (d <= g_radius && g_bladeSpeed > g.bestSpeed && t - g.bestAt < 0.05) {
                g.bestSpeed = g_bladeSpeed;
            }
            if (Melee_HasCredit(g.target)) g.touched = true;
        }
        // a dice miss on your blow never queues damage: judged after its Hit (blade on blade with its parry, or nothing)
        if (g.mine && g.read && g.verdict == 0 && !g.queued && g.due > 0 && t > g.due + 0.2) {
            bool met = g.best <= g_radius + g_abBonus && g.bestSpeed >= 1.2f;
            if (g.dice == 1) {  // a hit whose damage event we did not see (died, or other)
                g.verdict = 6;
            } else if (met) {
                g.verdict = 5;
                g_stats.bladeOnBlade++, g_stats.mine++, g_stats.met++;
                Combat_HitPosture(g.target, 20, "blade on blade");
                float c[3];
                for (int k = 0; k < 3; k++) c[k] = (g.a[k] + g.b[k]) * 0.5f;
                FireOutcome(g.target, OC_BLADE_ON_BLADE, c);
                Log("gate: yours on %08lX (%s, dice miss) -> blade on blade: posture", g.target, g.anim.c_str());
            } else {
                g.verdict = 6;
                g_stats.mine++;
                Log("gate: yours on %08lX (%s, dice miss) -> nothing (blade %.2f m off)", g.target, g.anim.c_str(), g.best);
            }
            g.verdictAt = t;
        }
    }
    for (size_t i = 0; i < g_gates.size();)
        if (t - g_gates[i].rolled > 4.0) g_gates.erase(g_gates.begin() + i);
        else i++;

    // ripostes: cut through it on its half-beat
    for (Riposte& r : g_ripostes) {
        if (r.done) continue;
        if (g_bladeValid && t >= r.due - 0.3 && t <= r.due + 0.15) {
            float d = SegSeg(g_bladeBase, g_bladeTip, r.a, r.b);
            if (d < r.best && g_bladeSpeed > 1.2f) r.best = d, r.bestAt = t;
        }
        if (t > r.due + 0.15) {
            r.done = true;
            if (r.best <= g_radius * 1.2f) {
                g_stats.ripostes++;
                Combat_HitPosture(r.target, 35, "riposte");
                float c[3];
                for (int k = 0; k < 3; k++) c[k] = (r.a[k] + r.b[k]) * 0.5f;
                Anim_Flinch(r.target, c);
                FireOutcome(r.target, OC_RIPOSTE, c);
                Log("fight: riposte lands on %08lX", r.target);
            }
        }
    }
    for (size_t i = 0; i < g_ripostes.size();)
        if (g_ripostes[i].done && t - g_ripostes[i].due > 1.0) g_ripostes.erase(g_ripostes.begin() + i);
        else i++;

    // charge glows: a beat before each engaged enemy's round rolls
    {
        float pp[3];
        bool have = Weapon_PlayerPos(pp);
        for (auto it = g_charge.begin(); it != g_charge.end();)
            it = t - it->second.lastRoll > 20 && it->second.predicted < 0 ? g_charge.erase(it) : std::next(it);
        static double lastScan;
        if (have && t - lastScan > 0.05) {
            lastScan = t;
            float e[3];
            DWORD nearId = Melee_NearestEnemy(pp, e);
            (void)nearId;
            for (auto& kv : g_charge) {
                float left;
                if (!RoundLeft(kv.first, &left)) continue;
                kv.second.predicted = t + left;
                if (left <= 0.75f && kv.second.shownAt < 0) kv.second.shownAt = t;
            }
        }
    }

    // the breath beat: your round's last 0.75 s, while you fight
    {
        float left;
        DWORD me = PlayerId();
        bool fighting = Combat_PlayerAttacking() && me && RoundLeft(me, &left);
        bool breath = fighting && left <= 0.75f && left > 0.02f;
        if (breath && !g_breath) {
            g_breathRoll = t + left;
            for (int s = ST_HIGH; s < ST_COUNT; s++) {
                g_stanceAvail[s] = false;
                g_stanceFeat[s] = 0;
                for (WORD f : kStanceFeats[s])
                    if (f && CanUse(f)) {
                        g_stanceAvail[s] = true;
                        g_stanceFeat[s] = f;
                        break;
                    }
            }
            Cues_Breath();
            if (g_bot) {  // the bot cycles the stances its build has, one per round
                static const Stance order[4] = {ST_HIGH, ST_THRUST, ST_LOW, ST_NEUTRAL};
                for (int k = 0; k < 4; k++) {
                    Stance s = order[(g_botStanceTurn + k) % 4];
                    if (s == ST_NEUTRAL || g_stanceAvail[s]) {
                        g_botStance = s;
                        g_botStanceTurn = (g_botStanceTurn + k + 1) % 4;
                        break;
                    }
                }
                Log("fight: bot holds the %s stance this breath", kStanceName[g_botStance]);
            }
        }
        g_breath = breath;
        // read the stance (a parry due within 0.4 s pauses it, never clears it)
        bool parryDue = false;
        for (const Gate& g : g_gates)
            if (!g.mine && g.due > t && g.due - t < 0.4) parryDue = true;
        if (g_breath && !parryDue) {
            Stance s = ReadStance();
            if (s != g_stanceCand) {
                g_stanceCand = s;
                g_stanceSince = t;
            }
            bool avail = s == ST_NEUTRAL || (s >= ST_HIGH && g_stanceAvail[s]);
            if (avail && s != g_stance && t - g_stanceSince >= 0.2) {
                g_stance = s;
                g_stanceFlashAt = t;
                g_stats.stances++;
                if (s >= ST_HIGH) {
                    WORD feat = g_stanceFeat[s];
                    BYTE key = QueueFeat(feat);
                    Cues_StanceLock(s - ST_HIGH + 1, kStanceRgb[s]);
                    if (key) {
                        g_tapKey = key;
                        g_queuedFeat = feat;
                        g_queuedAt = t;
                        g_stats.featsQueued++;
                        Log("fight: %s stance -> feat %u queued (column key %d)", kStanceName[s], feat, key - 1);
                    } else {
                        Log("fight: %s stance -> feat %u not in the action menu", kStanceName[s], feat);
                    }
                } else {
                    Log("fight: neutral stance");
                }
            }
        }
        if (!fighting) g_stance = ST_NONE;
    }

    if (g_gradeUntil > 0 && t > g_gradeUntil) {
        g_gradeUntil = -1;
        VR_HideText(3);
    }
    // the fight ends 5 s after the last blow either way
    if (g_stats.start > 0 && t - g_stats.last > 5.0) Fight_Summary("quiet");
    for (size_t i = 0; i < g_flashes.size();)
        if (t - g_flashes[i].at > 0.6) g_flashes.erase(g_flashes.begin() + i);
        else i++;
}

void Fight_Opening(DWORD id) {
    g_stats.openings++;
    FireOutcome(id, OC_OPENING, nullptr);
}

BYTE Fight_TakeKey() {
    BYTE k = g_tapKey;
    g_tapKey = 0;
    return k;
}

void Fight_Draw(const float wv[16]) {
    if (!g_active || !GlReady() || !Features().cues) return;
    double t = Now();
    if (gLW) gLW(3.0f);
    // ---- gates
    for (const Gate& g : g_gates) {
        if (!g.havePose || g.due < 0) continue;
        float left = (float)(g.due - t);
        bool resolved = g.verdict != 0;
        float age = resolved ? (float)(t - g.verdictAt) : 0;
        if (left > 1.6f || (resolved && age > 0.45f) || (!resolved && left < -0.25f)) continue;
        float ea[3], eb[3];
        ToEye(wv, g.a, ea);
        ToEye(wv, g.b, eb);
        {
            static double lastLog;
            static int drawn, behind;
            if (ea[2] > -0.1f || eb[2] > -0.1f) behind++;
            else drawn++;
            if (g_animLog && t - lastLog > 2.0 && drawn + behind) {  // ([Debug] AnimLog=1)
                Log("fight: gates drawn %d, behind the eye %d (last: %s, eye z %.2f / %.2f, %.2f s left)", drawn, behind,
                    g.mine ? "yours" : "theirs", ea[2], eb[2], left);
                lastLog = t;
                drawn = behind = 0;
            }
        }
        if (ea[2] > -0.1f || eb[2] > -0.1f) continue;
        float rgb[3];
        float alpha = 1;
        bool window = left <= g_window && left > -0.12f;
        if (resolved) {
            int v = g.verdict;
            bool good = v == 1 || v == 2 || v == 5 || v == 7 || v == 8;
            rgb[0] = good ? 1.0f : 0.6f, rgb[1] = good ? 0.95f : 0.25f, rgb[2] = good ? 0.75f : 0.2f;
            alpha = 1 - age / 0.45f;
        } else if (g.mine) {  // your gate: gold, solid in its window
            rgb[0] = 1.0f, rgb[1] = 0.78f, rgb[2] = 0.3f;
            alpha = window ? 1.0f : 0.55f;
        } else {  // theirs: blue, gold in the parry window; red when it can't be blocked (your hands are empty)
            bool blockable = Weapon_Right() != WC_NONE && Weapon_Profile(Weapon_Right()).melee;
            rgb[0] = blockable ? (window ? 1.0f : 0.35f) : 1.0f;
            rgb[1] = blockable ? (window ? 0.85f : 0.65f) : 0.2f;
            rgb[2] = blockable ? (window ? 0.4f : 1.0f) : 0.15f;
            alpha = window ? 1.0f : 0.6f;
        }
        Ribbon(ea, eb, window ? 0.02f : 0.012f, rgb[0], rgb[1], rgb[2], 0.9f * alpha);
        Ribbon(ea, eb, 0.04f, rgb[0], rgb[1], rgb[2], 0.15f * alpha);
        if (!resolved && left > 0) {  // the ring closes onto the gate at its Hit
            float c[3] = {(ea[0] + eb[0]) * 0.5f, (ea[1] + eb[1]) * 0.5f, (ea[2] + eb[2]) * 0.5f};
            float half = sqrtf((eb[0] - ea[0]) * (eb[0] - ea[0]) + (eb[1] - ea[1]) * (eb[1] - ea[1])) * 0.5f;
            float total = (float)(g.due - g.rolled);
            float frac = total > 0.05f ? Clamp(left / total, 0, 1) : 0;
            Ring(c, half * (1.0f + 1.6f * frac), rgb[0], rgb[1], rgb[2], 0.65f * alpha);
        }
    }
    // ---- riposte gates
    for (const Riposte& r : g_ripostes) {
        if (r.done) continue;
        float left = (float)(r.due - t);
        if (left > 0.6f || left < -0.15f) continue;
        float ea[3], eb[3];
        ToEye(wv, r.a, ea);
        ToEye(wv, r.b, eb);
        if (ea[2] > -0.1f || eb[2] > -0.1f) continue;
        Ribbon(ea, eb, 0.022f, 1.0f, 1.0f, 0.85f, 0.95f);
        Ribbon(ea, eb, 0.05f, 1.0f, 0.9f, 0.5f, 0.2f);
    }
    // ---- charge glows: a ring closing on the enemy's weapon hand over its last beat before the roll
    for (auto& kv : g_charge) {
        if (kv.second.shownAt < 0 || kv.second.predicted < 0) continue;
        float left = (float)(kv.second.predicted - t);
        if (left < -0.05f || left > 0.8f) continue;
        void* gob = GobOf(kv.first);
        float hand[3], e[3];
        if (!gob || !Anim_PartPos(gob, "rhand", hand)) {
            if (!CreaturePos(kv.first, hand)) continue;
            hand[2] += 1.3f;
        }
        ToEye(wv, hand, e);
        if (e[2] > -0.2f) continue;
        float k = Clamp(1 - left / 0.75f, 0, 1);
        Ring(e, 0.05f + 0.25f * (1 - k), 1.0f, 0.55f, 0.2f, 0.25f + 0.6f * k);
        Ring(e, 0.03f, 1.0f, 0.8f, 0.5f, 0.7f * k);
    }
    // ---- status bands over engaged enemies: health arc, posture segments, the opening's gold
    {
        float pp[3];
        if (Weapon_PlayerPos(pp)) {
            std::set<DWORD> seen;
            for (const Gate& g : g_gates) seen.insert(g.mine ? g.target : g.attacker);
            for (auto& kv : g_charge) seen.insert(kv.first);
            float e0[3];
            DWORD nearId = Melee_NearestEnemy(pp, e0);
            if (nearId) seen.insert(nearId);
            for (DWORD id : seen) {
                if (!Melee_IsEnemy(id)) continue;
                float p[3];
                if (!CreaturePos(id, p)) continue;
                float dx = p[0] - pp[0], dy = p[1] - pp[1];
                if (dx * dx + dy * dy > 12 * 12) continue;
                float top[3] = {p[0], p[1], p[2] + 2.05f}, e[3];
                {  // just over its head (kinrath, droids and children are not 2 m tall)
                    void* gob = GobOf(id);
                    float hh[3];
                    if (gob && (Anim_PartPos(gob, "headhook", hh) || Anim_PartPos(gob, "head_g", hh))) top[2] = hh[2] + 0.32f;
                }
                ToEye(wv, top, e);
                if (e[2] > -0.3f) continue;
                short hp, mhp;
                float w = 0.32f;
                if (HpOf(id, &hp, &mhp)) {
                    float frac = Clamp(hp / (float)mhp, 0, 1);
                    float a[3] = {e[0] - w, e[1], e[2]}, b[3] = {e[0] + w, e[1], e[2]}, m[3] = {e[0] - w + 2 * w * frac, e[1], e[2]};
                    Ribbon(a, b, 0.012f, 0.25f, 0.08f, 0.08f, 0.6f);
                    Ribbon(a, m, 0.012f, 0.95f, 0.25f, 0.18f, 0.95f);
                }
                float v, mx, openLeft;
                if (Combat_PostureInfo(id, &v, &mx, &openLeft)) {
                    int segs = 6;
                    float y = e[1] - 0.035f;
                    for (int s = 0; s < segs; s++) {
                        float x0 = e[0] - w + (2 * w) * s / segs + 0.006f, x1 = e[0] - w + (2 * w) * (s + 1) / segs - 0.006f;
                        float fill = Clamp(v / mx * segs - s, 0, 1);
                        float a[3] = {x0, y, e[2]}, b[3] = {x1, y, e[2]};
                        if (openLeft > 0) Ribbon(a, b, 0.012f, 1.0f, 0.8f, 0.3f, 0.6f + 0.35f * (float)sin(t * 12));
                        else Ribbon(a, b, 0.009f, 0.95f, 0.85f, 0.55f, 0.15f + 0.75f * fill);
                    }
                }
            }
        }
    }
    // ---- outcome flashes at the blow
    for (const Flash& f : g_flashes) {
        float e[3], age = (float)(t - f.at);
        ToEye(wv, f.pos, e);
        if (e[2] > -0.1f) continue;
        float k = 1 - age / 0.6f;
        Ring(e, f.size * (0.3f + age * 1.5f), f.rgb[0], f.rgb[1], f.rgb[2], 0.9f * k);
        Ring(e, f.size * (0.15f + age * 0.8f), f.rgb[0], f.rgb[1], f.rgb[2], 0.6f * k);
    }
    // ---- stance ghosts in the breath beat (fading to a glint after a few fights)
    if (g_breath && g_ghosts) {
        float eye[3], f[3], r[3];
        if (Facing(eye, f, r)) {
            float fade = g_fightsSeen < 5 ? 1.0f : 0.3f;
            for (int s = ST_HIGH; s < ST_COUNT; s++) {
                if (!g_stanceAvail[s]) continue;
                float h[3], d[3], tipW[3], eh[3], et[3];
                StanceGhost((Stance)s, eye, f, r, h, d);
                for (int k = 0; k < 3; k++) tipW[k] = h[k] + d[k] * 0.9f;
                ToEye(wv, h, eh);
                ToEye(wv, tipW, et);
                if (eh[2] > -0.05f || et[2] > -0.05f) continue;
                bool on = g_stanceCand == s, locked = g_stance == s;
                float a = (locked ? 0.9f : on ? 0.6f : 0.28f) * fade;
                Ribbon(eh, et, locked ? 0.016f : 0.01f, kStanceRgb[s][0], kStanceRgb[s][1], kStanceRgb[s][2], a);
                float gc[3] = {et[0], et[1] + 0.06f, et[2]};
                Glyph((Stance)s, gc, 0.035f, kStanceRgb[s], a + 0.1f);
            }
        }
    }
    // ---- low health: a red rim at the edge of the view (pulses)
    {
        short hp, mhp;
        DWORD me = PlayerId();
        if (me && HpOf(me, &hp, &mhp) && hp > 0 && hp < mhp * 0.35f) {
            float k = 0.25f + 0.2f * (float)sin(t * 6.0);
            float c[3] = {0, 0, -0.5f};
            if (gLW) gLW(14.0f);
            Ring(c, 0.36f, 0.9f, 0.05f, 0.05f, k, 48);
            if (gLW) gLW(3.0f);
        }
    }
    // ---- your guard on the hilt: four pips, dimming as it drains
    if (g_bladeValid) {
        float g = Combat_Guard();
        float eb[3];
        ToEye(wv, g_bladeBase, eb);
        if (eb[2] < -0.1f)
            for (int i = 0; i < 4; i++) {
                float fill = Clamp(g / 25.0f - i, 0, 1);
                float c[3] = {eb[0] + 0.035f * (i - 1.5f), eb[1] - 0.05f, eb[2]};
                Ring(c, 0.008f, g < 30 ? 1.0f : 0.5f, g < 30 ? 0.3f : 0.8f, g < 30 ? 0.2f : 1.0f, 0.2f + 0.7f * fill, 8);
            }
    }
}
