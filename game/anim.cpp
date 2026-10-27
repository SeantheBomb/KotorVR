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

#include "combat.h"
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

extern "C" void __cdecl Anim_AnimatePre(BYTE* gob, float* dt) {
    double t = Now();
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

__declspec(naked) static void AnimateStub() {
    __asm {
        push ecx
        lea eax, [esp + 8]            // &dt (first stack argument)
        push eax
        push ecx                      // the Gob
        call Anim_AnimatePre
        add esp, 8
        pop ecx
        jmp dword ptr [g_animateTramp]
    }
}

bool Anim_Install() {
    const char* ini = Features_IniPath();
    g_enabled = GetPrivateProfileIntA("Feel", "HitPause", 1, ini) != 0;
    g_defaultPause = GetPrivateProfileIntA("Feel", "HitPauseMs", 60, ini) / 1000.0f;
    if (!g_enabled) {
        Log("anim: hit-pause off ([Feel] HitPause=0)");
        return false;
    }
    return Combat_Detour(kAnimateVA, kAnimateOrig, 8, (void*)AnimateStub, &g_animateTramp, "Gob::Animate");
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
