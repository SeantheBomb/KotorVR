// Client animation hooks (v2 Phase A).
//
// Gob::Animate (0x486670, thiscall, float dt; names from Lane's K1 Ghidra labels, docs/FINDINGS.md "Client animation
// nodes") advances every model instance's animation stack once a frame. Scaling its dt gives a per-creature hit-pause:
// when the player's blow or bolt lands, the struck creature's models freeze for 40-90 ms (Until You Fall: hit-pause
// survives VR fine). Only the visual animation pauses - the server's combat clock and the d20 are untouched, and the
// lost time is not caught up (the next animation starts from the server's state anyway).
//
// The struck creature's model is recognised by position: a Gob's world position (+0x78) is in the same frame as the
// server creature's (+0x90), so a root model standing on the target's feet is its body. Weapons and heads are child
// models on its nodes, so they hold still with it.
//
// kvr.ini [Feel] HitPause=1 (default), HitPauseMs=60.
//
// The controlled character's own combat animation events are dropped: the hidden body still plays KotOR's attack
// animations on the round's schedule, and their swing whooshes, clash / parry sounds and saber blur fired out of step
// with what your hands do. Events reach a model through its virtual FireEvent (vtable +0x16C, thiscall (name, 0),
// called from 0x4826B0 inside Gob::Animate); the creature registers SwingShort / SwingLong / SwingTwirl / Clash /
// HitParry / blur_start / blur_end (0x61AB40). That slot is patched on the player body's model class and filters only
// that one model; Contact (the hit landing), footsteps and doneattack (game logic) still run.
#include "anim.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <map>

#include "combat.h"
#include "dialogue.h"
#include "enginecam.h"
#include "features.h"
#include "weapons.h"

extern void Log(const char* fmt, ...);

namespace {

const DWORD kAnimateVA = 0x486670;
const BYTE kAnimateOrig[8] = {0x6A, 0xFF, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00};  // push -1; mov eax, fs:[0]

BYTE* g_animateTramp;
bool g_enabled = true;
float g_defaultPause = 0.06f;

struct Pause {
    float pos[3];
    double until;
    DWORD id;
};
Pause g_pause[4];
bool g_lookAtYou = true;  // [Feel] LookAtYou

typedef void(__fastcall* FireEventFn)(BYTE* gob, void* edx, const char* name, int arg);  // thiscall, ret 8
FireEventFn g_fireOrig;
DWORD* g_firePatchedVtbl;
BYTE* volatile g_playerGob;

bool Suppressed(const char* n) {
    static const char* kDrop[] = {"SwingShort", "SwingLong", "SwingTwirl", "Clash", "HitParry", "blur_start", "blur_end"};
    for (const char* d : kDrop)
        if (_stricmp(n, d) == 0) return true;
    return false;
}

void __fastcall FireEventHook(BYTE* gob, void* edx, const char* name, int arg) {
    if (gob == g_playerGob && name) {
        bool drop = false;
        __try {
            drop = Suppressed(name);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (drop) {
            static int logged;
            if (logged++ < 10) Log("anim: dropped the hidden body's '%s' event", name);
            return;
        }
    }
    g_fireOrig(gob, edx, name, arg);
}

BYTE* PlayerBodyGob() {
    typedef void*(__fastcall * GetPlayerCreatureFn)(void* clientApp, void*);
    typedef void*(__fastcall * GetModelFn)(void* creature, void*, int part, int create);
    __try {
        void* app = *(void**)0x7A39FC;
        void* client = app ? *(void**)((char*)app + 4) : nullptr;
        void* creature = client ? ((GetPlayerCreatureFn)0x5ED540)(client, nullptr) : nullptr;
        return creature ? (BYTE*)((GetModelFn)0x6179C0)(creature, nullptr, 0xFF, 0) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

double Now() {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / f.QuadPart;
}

}  // namespace

// Every model animated recently (v2 per-bone hit volumes find a creature's model by its position). Gobs that stop
// being animated (freed, unloaded) drop out after a second; reads are guarded anyway.
std::map<BYTE*, DWORD> g_seenGobs;

extern "C" void __cdecl Anim_AnimatePre(BYTE* gob, float* dt) {
    double t = Now();
    g_seenGobs[gob] = GetTickCount();
    for (const Pause& p : g_pause) {
        if (t >= p.until) continue;
        __try {
            const float* g = (const float*)(gob + 0x78);
            float dx = g[0] - p.pos[0], dy = g[1] - p.pos[1], dz = g[2] - p.pos[2];
            if (dx * dx + dy * dy < 0.2f * 0.2f && fabsf(dz) < 0.3f) {
                *dt = 0;
                return;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// After a model's animation is sampled: procedural node overrides (NPCs looking at you).
extern "C" void __cdecl Anim_AnimatePost(BYTE* gob);

__declspec(naked) static void AnimateStub() {
    __asm {
        push ecx
        lea eax, [esp + 8]            // &dt (first stack argument)
        push eax
        push ecx                      // the Gob
        call Anim_AnimatePre
        add esp, 8
        pop ecx
        push ecx                      // keep the Gob for the post step
        push dword ptr [esp + 8]      // dt (as possibly scaled by the pre step)
        call dword ptr [g_animateTramp]  // thiscall, pops dt
        pop ecx
        push ecx
        call Anim_AnimatePost
        add esp, 4
        ret 4
    }
}

bool Anim_Install() {
    const char* ini = Features_IniPath();
    g_enabled = GetPrivateProfileIntA("Feel", "HitPause", 1, ini) != 0;
    g_defaultPause = GetPrivateProfileIntA("Feel", "HitPauseMs", 60, ini) / 1000.0f;
    g_lookAtYou = GetPrivateProfileIntA("Feel", "LookAtYou", 1, ini) != 0;
    if (!g_enabled) {
        Log("anim: hit-pause off ([Feel] HitPause=0)");
        return false;
    }
    return Combat_Detour(kAnimateVA, kAnimateOrig, 8, (void*)AnimateStub, &g_animateTramp, "Gob::Animate");
}

// ---------------------------------------------------------------- NPCs look at you
// The head node of a creature model: "headhook" (the head model hangs on it), found by name in its Part tree.
typedef const char*(__fastcall* PartNameFn)(void* part, void*);
typedef void(__fastcall* PartGetWorldFn)(void* part, void*, float* pos, float* quat);

void* FindPart(void* part, const char* name, int depth) {
    if (!part || depth > 40) return nullptr;
    const char* n = ((PartNameFn)0x440200)(part, nullptr);
    if (n && _stricmp(n, name) == 0) return part;
    void** kids = *(void***)((char*)part + 0x30);
    int nk = *(int*)((char*)part + 0x34);
    if (!kids || nk < 0 || nk > 64) return nullptr;
    for (int i = 0; i < nk; i++)
        if (void* f = FindPart(kids[i], name, depth + 1)) return f;
    return nullptr;
}

// quaternions (w, x, y, z), as KotOR's Part stores them (+0x14)
void QMul(const float* a, const float* b, float* o) {
    float r[4] = {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3], a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
                  a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1], a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
    memcpy(o, r, sizeof r);
}
void QConj(const float* a, float* o) { o[0] = a[0]; o[1] = -a[1]; o[2] = -a[2]; o[3] = -a[3]; }
void QRot(const float* q, const float* v, float* o) {  // q v q*
    float p[4] = {0, v[0], v[1], v[2]}, c[4], t[4];
    QConj(q, c);
    QMul(q, p, t);
    QMul(t, c, t);
    o[0] = t[1]; o[1] = t[2]; o[2] = t[3];
}

// NPCs look at you in conversations (2026-10-27 playtest): after a creature model within 10 m is animated, its head
// node ("headhook", the head model hangs on it; forward = +Y, measured on Trask) is turned toward your eye - at most
// 70 deg of yaw and 25 deg of pitch, all the way, not when you stand behind them.
// The node is not keyframed in most animations, so the engine does not reset it each frame: the turn is always
// computed from the engine's own value (remembered until the engine writes a new one), never stacked on our last one.
// kvr.ini [Feel] LookAtYou=0 off.
struct HeadState {
    void* head;
    float base[4];     // the engine's local rotation
    float written[4];  // what we last wrote
    DWORD seen;
};
HeadState g_heads[16];

void LookAt(BYTE* gob, void* head, const float eye[3]) {
    float* lq = (float*)((char*)head + 0x14);
    HeadState* hs = nullptr;
    for (HeadState& h : g_heads)
        if (h.head == head) hs = &h;
    if (!hs) {  // the stalest slot
        hs = &g_heads[0];
        for (HeadState& h : g_heads)
            if (h.seen < hs->seen) hs = &h;
        hs->head = head;
        memcpy(hs->base, lq, 16);
        memset(hs->written, 0, 16);
    } else if (memcmp(lq, hs->written, 16) != 0) {
        memcpy(hs->base, lq, 16);  // the engine wrote a new value
    }
    hs->seen = GetTickCount();
    float wp[3], wq[4];
    ((PartGetWorldFn)0x43E4A0)(head, nullptr, wp, wq);
    // the parent's world rotation P = W * local^-1 (W from the current local), then the head as the engine posed it
    float lc[4], P[4], Pc[4], W[4];
    QConj(lq, lc);
    QMul(wq, lc, P);
    QConj(P, Pc);
    QMul(P, hs->base, W);
    const float fy[3] = {0, 1, 0};
    float F[3], T[3] = {eye[0] - wp[0], eye[1] - wp[1], eye[2] - wp[2]};
    QRot(W, fy, F);
    float fl = sqrtf(F[0] * F[0] + F[1] * F[1]), tl = sqrtf(T[0] * T[0] + T[1] * T[1]);
    if (fl < 0.2f || tl < 0.05f) {
        memcpy(lq, hs->base, 16);
        memcpy(hs->written, lq, 16);
        return;
    }
    float yaw = atan2f(F[0] * T[1] - F[1] * T[0], F[0] * T[0] + F[1] * T[1]);  // signed, F -> T about +Z
    float out[4];
    if (fabsf(yaw) > 1.75f) {  // you are behind them (> 100 deg): leave the head alone
        memcpy(out, hs->base, 16);
    } else {
        float turn = yaw;
        turn = turn > 1.22f ? 1.22f : turn < -1.22f ? -1.22f : turn;  // 70 deg
        float pitchNow = atan2f(F[2], fl), pitchTo = atan2f(T[2], tl);
        float pt = pitchTo - pitchNow;
        pt = pt > 0.44f ? 0.44f : pt < -0.44f ? -0.44f : pt;  // 25 deg
        // pitch about the head's own sideways axis (horizontal, perpendicular to its facing), then yaw about world up
        float side[3] = {F[1] / fl, -F[0] / fl, 0};
        float dp[4] = {cosf(pt / 2), side[0] * sinf(pt / 2), side[1] * sinf(pt / 2), 0};  // + about F x up = tilt up
        float dy[4] = {cosf(turn / 2), 0, 0, sinf(turn / 2)};
        float d[4], t[4];
        QMul(dy, dp, d);
        // local' = P^-1 * D * P * base
        QMul(d, P, t);
        QMul(Pc, t, t);
        QMul(t, hs->base, out);
        float n = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
        if (!(n > 0.5f && n < 1.5f)) {
            memcpy(out, hs->base, 16);
        } else {
            for (float& v : out) v /= n;
        }
    }
    memcpy(lq, out, 16);
    memcpy(hs->written, out, 16);
}

// ---------------------------------------------------------------- flinch (v2 phase F reactions)
// A blade / bolt contact makes the struck creature's upper torso ("torsoUpr_g") lean away from the hit, 14 deg up and
// back over 0.25 s, on top of whatever the engine animates (same engine-value cache as the head turn).
struct Flinch {
    float pos[3];   // the creature's feet (identifies its model)
    float axis[3];  // world rotation axis (horizontal, perpendicular to the push)
    double start;
};
Flinch g_flinch[6];
struct NodeState {
    void* part;
    float base[4], written[4];
    DWORD seen;
};
NodeState g_torsos[16];

void WorldDelta(NodeState* cache, int n, void* part, const float d[4]) {
    float* lq = (float*)((char*)part + 0x14);
    NodeState* hs = nullptr;
    for (int i = 0; i < n; i++)
        if (cache[i].part == part) hs = &cache[i];
    if (!hs) {
        hs = &cache[0];
        for (int i = 0; i < n; i++)
            if (cache[i].seen < hs->seen) hs = &cache[i];
        hs->part = part;
        memcpy(hs->base, lq, 16);
        memset(hs->written, 0, 16);
    } else if (memcmp(lq, hs->written, 16) != 0) {
        memcpy(hs->base, lq, 16);
    }
    hs->seen = GetTickCount();
    float wp[3], wq[4], lc[4], P[4], Pc[4], t[4], out[4];
    ((PartGetWorldFn)0x43E4A0)(part, nullptr, wp, wq);
    QConj(lq, lc);
    QMul(wq, lc, P);
    QConj(P, Pc);
    QMul(d, P, t);
    QMul(Pc, t, t);
    QMul(t, hs->base, out);
    float nn = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
    if (!(nn > 0.5f && nn < 1.5f)) memcpy(out, hs->base, 16);
    else
        for (float& v : out) v /= nn;
    memcpy(lq, out, 16);
    memcpy(hs->written, out, 16);
}

void ApplyFlinch(BYTE* gob) {
    double t = Now();
    const float* gp = (const float*)(gob + 0x78);
    for (Flinch& f : g_flinch) {
        double age = t - f.start;
        if (age < 0 || age > 0.25) continue;
        float dx = gp[0] - f.pos[0], dy = gp[1] - f.pos[1], dz = gp[2] - f.pos[2];
        if (dx * dx + dy * dy > 0.25f * 0.25f || fabsf(dz) > 0.4f) continue;
        void* torso = FindPart(*(void**)(gob + 0x6C), "torsoUpr_g", 0);
        if (!torso) return;
        float a = 0.244f * (float)sin(age / 0.25 * 3.14159265);  // 14 deg peak
        float s = sinf(a / 2);
        float d[4] = {cosf(a / 2), f.axis[0] * s, f.axis[1] * s, f.axis[2] * s};
        WorldDelta(g_torsos, 16, torso, d);
        return;
    }
}

extern "C" void __cdecl Anim_AnimatePost(BYTE* gob) {
    __try {
        ApplyFlinch(gob);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!g_lookAtYou || !Dialogue_Showing() || gob == g_playerGob) return;
    float eye[3];
    if (!EngineCam_EyeWorld(eye)) return;
    __try {
        const float* gp = (const float*)(gob + 0x78);
        float dx = eye[0] - gp[0], dy = eye[1] - gp[1];
        if (dx * dx + dy * dy > 10.0f * 10.0f) return;
        void* head = FindPart(*(void**)(gob + 0x6C), "headhook", 0);
        if (head) LookAt(gob, head, eye);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Anim_Update() {
    if (!Features().modelCull) return;
    BYTE* gob = PlayerBodyGob();
    g_playerGob = gob;
    if (!gob) return;
    DWORD* vt = nullptr;
    __try {
        vt = *(DWORD**)gob;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!vt || vt == g_firePatchedVtbl || (ULONG_PTR)vt < 0x700000 || (ULONG_PTR)vt > 0x7A0000) return;
    if (g_firePatchedVtbl) return;  // one class patched; a different body class keeps its sounds (logged once)
    DWORD* slot = vt + 0x16C / 4;
    g_fireOrig = (FireEventFn)*slot;
    DWORD old;
    VirtualProtect(slot, 4, PAGE_READWRITE, &old);
    *slot = (DWORD)(ULONG_PTR)FireEventHook;
    VirtualProtect(slot, 4, old, &old);
    g_firePatchedVtbl = vt;
    Log("anim: the hidden body's combat events filtered (model vtable %p, FireEvent %p)", vt, g_fireOrig);
}

void Anim_Flinch(unsigned long targetId, const float hit[3]) {
    __try {
        BYTE* c = (BYTE*)Weapon_CreatureById(targetId);
        if (!c) return;
        Flinch* slot = &g_flinch[0];
        for (Flinch& f : g_flinch)
            if (f.start < slot->start) slot = &f;
        memcpy(slot->pos, c + 0x90, 12);
        // push: from the hit point toward the body's centre line (horizontal); lean away = rotate about up x push
        float px = slot->pos[0] - hit[0], py = slot->pos[1] - hit[1];
        float l = sqrtf(px * px + py * py);
        if (l < 1e-3f) { px = 1; py = 0; l = 1; }
        px /= l;
        py /= l;
        slot->axis[0] = -py;  // (0,0,1) x (px,py,0) = (-py, px, 0): tips the top along the push
        slot->axis[1] = px;
        slot->axis[2] = 0;
        slot->start = Now();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void* Anim_ModelAt(const float pos[3]) {
    DWORD now = GetTickCount();
    BYTE* best = nullptr;
    float bd = 0.25f * 0.25f;
    for (auto it = g_seenGobs.begin(); it != g_seenGobs.end();) {
        if (now - it->second > 1000) {
            it = g_seenGobs.erase(it);
            continue;
        }
        __try {
            const float* g = (const float*)(it->first + 0x78);
            float dx = g[0] - pos[0], dy = g[1] - pos[1], dz = g[2] - pos[2];
            float d = dx * dx + dy * dy + dz * dz;
            if (d < bd && *(void**)(it->first + 0x6C)) {
                bd = d;
                best = it->first;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        ++it;
    }
    return best;
}

static bool IsHitBone(const char* n) {  // skeleton nodes, not hooks / dummies / effect points
    if (!n || !n[0]) return false;
    static const char* kSkip[] = {"hook", "dummy", "impact", "camera", "conjure", "lightsaber", "deflect", "bolt", "headtrack"};
    char low[64];
    int i = 0;
    for (; n[i] && i < 63; i++) low[i] = (char)tolower((unsigned char)n[i]);
    low[i] = 0;
    for (const char* k : kSkip)
        if (strstr(low, k)) return false;
    return true;
}

static void Bones(void* part, const float* parentPos, bool parentIsBone, AnimBone* out, int* n, int max, int depth) {
    if (!part || depth > 40 || *n >= max) return;
    float wp[3], wq[4];
    ((PartGetWorldFn)0x43E4A0)(part, nullptr, wp, wq);
    const char* name = ((PartNameFn)0x440200)(part, nullptr);
    bool bone = IsHitBone(name);
    if (bone && parentIsBone && parentPos) {
        float dx = wp[0] - parentPos[0], dy = wp[1] - parentPos[1], dz = wp[2] - parentPos[2];
        float len2 = dx * dx + dy * dy + dz * dz;
        if (len2 > 0.0004f && len2 < 1.5f * 1.5f) {  // a real limb segment (not a zero-length or a stray link)
            AnimBone& b = out[(*n)++];
            memcpy(b.a, parentPos, 12);
            memcpy(b.b, wp, 12);
            char low[24] = {};
            for (int i = 0; name[i] && i < 23; i++) low[i] = (char)tolower((unsigned char)name[i]);
            b.head = strstr(low, "head") != nullptr || strstr(low, "neck") != nullptr;
            b.radius = strstr(low, "torso") || strstr(low, "pelvis") || strstr(low, "spine") ? 0.2f : 0.12f;
        }
    }
    void** kids = *(void***)((char*)part + 0x30);
    int nk = *(int*)((char*)part + 0x34);
    if (!kids || nk < 0 || nk > 64) return;
    for (int i = 0; i < nk; i++) Bones(kids[i], bone ? wp : parentPos, bone || parentIsBone, out, n, max, depth + 1);
}

int Anim_BoneSegments(void* model, AnimBone* out, int max) {
    int n = 0;
    __try {
        Bones(*(void**)((BYTE*)model + 0x6C), nullptr, false, out, &n, max, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return n;
}

bool Anim_PlayerBodyPos(float out[3]) {
    BYTE* g = g_playerGob;
    if (!g) return false;
    __try {
        memcpy(out, g + 0x78, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

void Anim_HitPause(unsigned long targetId, float seconds) {
    if (!g_animateTramp) return;
    if (seconds <= 0) seconds = g_defaultPause;
    __try {
        BYTE* c = (BYTE*)Weapon_CreatureById(targetId);
        if (!c) return;
        Pause* slot = &g_pause[0];
        for (Pause& p : g_pause)
            if (p.until < slot->until) slot = &p;
        memcpy(slot->pos, c + 0x90, sizeof slot->pos);
        slot->id = targetId;
        slot->until = Now() + seconds;
        Log("anim: hit-pause %.0f ms on %08lX", seconds * 1000, targetId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
