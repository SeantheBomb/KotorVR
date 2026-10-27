// What the right hand points at in the world, named (2026-10-27 playtest: "provide the original tooltip").
//
// The flat HUD that showed KotOR's hover name is hidden in the world, so the name of the game's mouse-over object
// (weapons.cpp Weapon_HoverClientId - the cursor follows the hand) is shown as a small label just above the hand's
// aim ray, at the object's distance: the tooltip at the cursor, as in the original. A grip / shot on it flashes the
// label for a moment. Hidden in conversations, cutscenes and menus.
//
// The name: the client object's virtual GetName(CExoString* out) (CSWCCreature::GetName 0x617090 sits at vtable
// 0x74F668 in the client creature class; the slot is the same for every client object class, so it is computed once
// from the player's own client creature). Client objects by id: CClientExoApp::GetGameObject 0x5ED580.
#include "hover.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/kvr_shared.h"
#include "dialogue.h"
#include "enginecam.h"
#include "vr.h"
#include "weapons.h"

extern void Log(const char* fmt, ...);

namespace {

const int kSlot = 3;
int g_nameSlot = -1;  // vtable index of GetName, -1 unknown, -2 not found
DWORD g_shownId;
char g_name[96];
double g_flashUntil;
bool g_visible;

double Now() { return GetTickCount() / 1000.0; }

void* ClientObject(DWORD clientId) {
    typedef void*(__fastcall * GetGameObjectFn)(void* clientApp, void*, DWORD id);
    __try {
        BYTE* app = *(BYTE**)0x7A39FC;
        void* client = app ? *(void**)(app + 4) : nullptr;
        return client ? ((GetGameObjectFn)0x5ED580)(client, nullptr, clientId) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

void FindNameSlot() {
    typedef void*(__fastcall * GetPlayerCreatureFn)(void* clientApp, void*);
    __try {
        BYTE* app = *(BYTE**)0x7A39FC;
        void* client = app ? *(void**)(app + 4) : nullptr;
        void* pc = client ? ((GetPlayerCreatureFn)0x5ED540)(client, nullptr) : nullptr;
        if (!pc) return;
        DWORD* vt = *(DWORD**)pc;
        for (int i = 0; i < 300; i++)
            if (vt[i] == 0x617090) {
                g_nameSlot = i;
                Log("hover: GetName is vtable slot %d", i);
                return;
            }
        g_nameSlot = -2;
        Log("hover: GetName not found in the client creature vtable %p", vt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool NameOf(DWORD clientId, char* out, int cap) {
    out[0] = 0;
    if (g_nameSlot == -1) FindNameSlot();
    if (g_nameSlot < 0) return false;
    void* o = ClientObject(clientId);
    if (!o) return false;
    typedef void*(__fastcall * GetNameFn)(void* self, void*, void* outStr);
    __try {
        struct { char* p; int len; } s = {nullptr, 0};  // CExoString (leaks a few bytes per new hover; rare)
        ((GetNameFn)(*(DWORD**)o)[g_nameSlot])(o, nullptr, &s);
        if (s.p && s.len > 0) {
            int n = s.len < cap - 1 ? s.len : cap - 1;
            memcpy(out, s.p, n);
            out[n] = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
    }
    return out[0] != 0;
}

bool ObjectPos(DWORD serverId, float out[3]) {  // any server object (0x4AE750), world position +0x90
    typedef void*(__thiscall * GetObject_t)(void* server, DWORD id);
    __try {
        BYTE* app = *(BYTE**)0x7A39FC;
        BYTE* server = app ? *(BYTE**)(app + 8) : nullptr;
        BYTE* o = server ? (BYTE*)((GetObject_t)0x4AE750)(server, serverId) : nullptr;
        if (!o) return false;
        memcpy(out, o + 0x90, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

void Hide() {
    if (g_visible) VR_HideText(kSlot);
    g_visible = false;
    g_shownId = 0;
}

}  // namespace

void Hover_Interacted() { g_flashUntil = Now() + 0.6; }

void Hover_Update(const KvrInput& in) {
    const KvrShared* shm = VR_Shared();
    DWORD id;
    if (!shm || shm->cinematic || shm->menuOpen || Dialogue_Showing() || in.pointerMode != KVR_POINTER_WORLD ||
        !Weapon_HoverClientId(&id)) {
        Hide();
        return;
    }
    if (id != g_shownId) {
        g_shownId = id;
        if (!NameOf(id, g_name, sizeof g_name)) g_name[0] = 0;
    }
    if (!g_name[0]) {
        if (g_visible) VR_HideText(kSlot);
        g_visible = false;
        return;
    }
    // distance to the object, for where along the ray the label floats and how big it is
    float eye[3], dist = 3.0f;
    float op[3];
    bool havePos = ObjectPos(id & 0x7FFFFFFF, op);
    if (havePos && EngineCam_EyeWorld(eye)) {
        float dx = op[0] - eye[0], dy = op[1] - eye[1];
        dist = sqrtf(dx * dx + dy * dy);
    }
    dist = dist < 1.0f ? 1.0f : dist > 6.0f ? 6.0f : dist;
    // the aim ray (tracking space = the WORLD anchor's frame): x, y, z, w; forward -Z
    const float* q = in.aim.quat;
    float f[3] = {-(2 * (q[0] * q[2] + q[3] * q[1])), -(2 * (q[1] * q[2] - q[3] * q[0])), -(1 - 2 * (q[0] * q[0] + q[1] * q[1]))};
    float d = dist * 0.9f;
    float pos[3] = {in.aim.pos[0] + f[0] * d, in.aim.pos[1] + f[1] * d + 0.06f * dist + 0.08f, in.aim.pos[2] + f[2] * d};
    float yaw = VR_HeadYaw();
    float rq[4] = {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
    char text[128];
    bool flash = Now() < g_flashUntil;
    sprintf_s(text, flash ? "> %s <" : "%s", g_name);
    static char lastText[128];
    static DWORD lastSent;
    if (!g_visible || strcmp(text, lastText) != 0 || GetTickCount() - lastSent > 50) {  // not every frame
        VR_ShowText(kSlot, KVR_ANCHOR_WORLD, pos, rq, 0.09f * dist + 0.12f, text, -1);
        strcpy_s(lastText, text);
        lastSent = GetTickCount();
    }
    g_visible = true;
}
