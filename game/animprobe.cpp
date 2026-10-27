// [Debug] AnimProbe=1: read-only check of the client animation node layout (v2 spike, docs/ANIMATION.md).
//
// Every 5 s, walks the player creature's body model: CClientExoApp::GetPlayerCreature -> CSWCCreature::GetModel(0xFF)
// -> Gob root Part [gob+0x6C] -> children (CExoArrayList at part+0x30), and gets each Part's world position from the
// engine's own Part::GetWorld. Those are compared against the bone positions the GL hooks recorded for the hidden
// body this frame (VR_OwnNode): if they agree to ~1 cm, the Part layout (local pos +0x08, quat +0x14, parent +0x3C,
// gob +0x40) is right and writing it later moves what is drawn. Nothing is written.
// Addresses: Steam build, names from Lane's K1 Ghidra labels (identical to GOG here).
#include "animprobe.h"

#include <windows.h>

#include <cmath>
#include <cstring>

#include "features.h"
#include "vr.h"

extern void Log(const char* fmt, ...);

namespace {

typedef void*(__fastcall* GetPlayerCreatureFn)(void* clientApp, void*);
typedef void*(__fastcall* GetModelFn)(void* creature, void*, int part, int create);
typedef const char*(__fastcall* PartNameFn)(void* part, void*);
typedef void(__fastcall* PartGetWorldFn)(void* part, void*, float* pos, float* quat);

const auto kGetPlayerCreature = (GetPlayerCreatureFn)0x5ED540;  // CClientExoApp::GetPlayerCreature
const auto kGetModel = (GetModelFn)0x6179C0;                    // CSWCCreature::GetModel
const auto kPartName = (PartNameFn)0x440200;                    // Part::Name
const auto kPartGetWorld = (PartGetWorldFn)0x43E4A0;            // Part::GetWorld

struct Sample {
    char name[24];
    float world[3];
    float local[3];
    float quat[4];
    float nearest;  // distance to the closest GL-recorded bone, m
};

Sample g_s[96];
int g_n;
int g_total;

float Nearest(const float* p, const float (*nodes)[3], int count) {
    float best = 1e9f;
    for (int i = 0; i < count; i++) {
        float dx = p[0] - nodes[i][0], dy = p[1] - nodes[i][1], dz = p[2] - nodes[i][2];
        float d = dx * dx + dy * dy + dz * dz;
        if (d < best) best = d;
    }
    return sqrtf(best);
}

void Walk(void* part, int depth, const float (*nodes)[3], int count) {
    if (!part || depth > 40) return;
    g_total++;
    if (g_n < 96) {
        Sample& s = g_s[g_n++];
        const char* nm = kPartName(part, nullptr);
        strncpy_s(s.name, nm ? nm : "?", _TRUNCATE);
        float q[4];
        kPartGetWorld(part, nullptr, s.world, q);
        memcpy(s.local, (char*)part + 0x08, 12);
        memcpy(s.quat, (char*)part + 0x14, 16);
        s.nearest = Nearest(s.world, nodes, count);
    }
    void** kids = *(void***)((char*)part + 0x30);
    int nk = *(int*)((char*)part + 0x34);
    if (!kids || nk < 0 || nk > 64) return;
    for (int i = 0; i < nk; i++) {
        void* c = kids[i];
        if (c && *(void**)((char*)c + 0x3C) != part) {
            Log("animprobe: child %d of %s has parent %p, not %p", i, g_s[0].name, *(void**)((char*)c + 0x3C), part);
        }
        Walk(c, depth + 1, nodes, count);
    }
}

// SEH only (no C++ objects): a wrong guess faults here instead of crashing the game.
bool Probe(const float (*nodes)[3], int count, void** gobOut) {
    __try {
        void* app = *(void**)0x7A39FC;
        if (!app) return false;
        void* client = *(void**)((char*)app + 4);
        if (!client) return false;
        void* creature = kGetPlayerCreature(client, nullptr);
        if (!creature) return false;
        void* gob = kGetModel(creature, nullptr, 0xFF, 0);
        if (!gob) return false;
        *gobOut = gob;
        void* root = *(void**)((char*)gob + 0x6C);
        g_n = 0;
        g_total = 0;
        Walk(root, 0, nodes, count);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("animprobe: fault while walking the model (code %08lX)", GetExceptionCode());
        return false;
    }
}

}  // namespace

void AnimProbe_OnSwap() {
    if (!Features().animProbe) return;
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 5000) return;
    last = now;
    static float nodes[128][3];
    int count = VR_OwnNodesThisFrame(nodes, 128);
    void* gob = nullptr;
    if (!Probe(nodes, count, &gob)) return;
    int close = 0;
    float worst = 0, sum = 0;
    for (int i = 0; i < g_n; i++) {
        if (g_s[i].nearest < 0.01f) close++;
        if (g_s[i].nearest < 1e8f) { sum += g_s[i].nearest; if (g_s[i].nearest > worst) worst = g_s[i].nearest; }
    }
    Log("animprobe: gob %p, %d parts (%d sampled), GL bones %d: %d within 1 cm, mean %.3f m, worst %.3f m", gob,
        g_total, g_n, count, close, g_n ? sum / g_n : 0.f, worst);
    static int detailed;
    if (detailed++ < 2) {
        for (int i = 0; i < g_n; i++) {
            const Sample& s = g_s[i];
            Log("animprobe:   %-16s world (%.3f %.3f %.3f) local (%.3f %.3f %.3f) q (%.3f %.3f %.3f %.3f) gl %.3f",
                s.name, s.world[0], s.world[1], s.world[2], s.local[0], s.local[1], s.local[2], s.quat[0], s.quat[1],
                s.quat[2], s.quat[3], s.nearest);
        }
    }
}
