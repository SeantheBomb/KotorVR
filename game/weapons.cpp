// Equipped-weapon detection, weapon profiles and in-hand proxies (x86, inside swkotor.exe).
//
// Player and equipment come straight from the server-side game objects (Steam build addresses, see FINDINGS.md):
//   app      = [0x7A39FC];  server = [app + 8];  internal = [server + 4]
//   player   = first entry of [internal + 0x10064] (vtable[1] cast); controlled creature id = [player + 0x34]
//              (the party NPC after Tab, else the PC = [player + 0x38])
//   creature = 0x4AE770(server, id)          thiscall: object id -> server creature
//   item     = 0x5A4C20([creature + 0xA2C], slot)   thiscall: inventory -> item in slot (0x10 right, 0x20 left)
//   base     = [item + 0x0C]                 row in baseitems.2da
// Everything is read under SEH guards and refreshed four times a second.

#include "weapons.h"

#include <GL/gl.h>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../common/kvr_shared.h"
#include "vr.h"
#include "weaponmodels.h"
#include "features.h"
#include "ownsig.h"
#include "combat.h"
#include "cues.h"

extern void Log(const char* fmt, ...);

namespace {

const WeaponProfile kProfiles[WC_COUNT] = {
    // name              melee  2h     swing m/s  travel m
    {"unarmed",          true,  false, 2.2f,      0.30f},
    {"lightsaber",       true,  false, 2.0f,      0.30f},
    {"short lightsaber", true,  false, 1.8f,      0.25f},
    {"double lightsaber",true,  true,  2.0f,      0.35f},
    {"blade",            true,  false, 2.2f,      0.35f},
    {"short blade",      true,  false, 2.0f,      0.25f},
    {"double blade",     true,  true,  2.2f,      0.40f},
    {"stun baton",       true,  false, 2.4f,      0.25f},
    {"staff",            true,  true,  2.2f,      0.40f},
    {"pistol",           false, false, 0.0f,      0.0f},
    {"rifle",            false, true,  0.0f,      0.0f},
    {"repeater",         false, true,  0.0f,      0.0f},
};

// baseitems.2da row -> class (KotOR 1 stock rows; unknown rows fall back on nothing)
WeaponClass ClassOfBaseItem(int row) {
    switch (row) {
    case 0: case 78: return WC_STAFF;             // quarterstaff, ghaffi stick
    case 1: return WC_BATON;
    case 2: case 3: case 80: return WC_BLADE;     // long sword, vibro sword, gamorrean battleaxe
    case 4: case 5: return WC_BLADE_SHORT;
    case 6: case 7: case 79: return WC_BLADE_DOUBLE;  // double-bladed, vibro double-blade, wookiee warblade
    case 8: return WC_SABER;
    case 9: return WC_SABER_DOUBLE;
    case 10: return WC_SABER_SHORT;
    case 12: case 13: case 14: case 15: case 16: case 17: return WC_PISTOL;
    case 18: case 19: case 20: case 21: case 22: case 77: return WC_RIFLE;
    case 23: case 24: return WC_REPEATER;
    default: return WC_NONE;
    }
}

typedef void*(__thiscall* GetCreature_t)(void* server, DWORD id);
typedef void*(__thiscall* GetItemInSlot_t)(void* inventory, DWORD slot);
const GetCreature_t GetCreature = (GetCreature_t)0x4AE770;
const GetItemInSlot_t GetItemInSlot = (GetItemInSlot_t)0x5A4C20;
typedef void*(__thiscall* ListGetAt_t)(void* list, void* pos);
const ListGetAt_t ListGetAt = (ListGetAt_t)0x5E9B60;  // CExoLinkedList::GetAtPos

void* g_player;
WeaponClass g_right = WC_NONE, g_left = WC_NONE;
int g_rightRow = -1, g_leftRow = -1;
int g_rightVar = 1, g_leftVar = 1;  // item model variation (w_<class>_<NNN>)
// the controlled character's body (for the hands): appearance.2da row, armor body letter (0 = a .. 9 = j), texture var
int g_appearance = -1, g_bodyLetter = 0, g_bodyTexVar = 1;
std::string g_handModel, g_handTexture;

int RowInSlot(void* creature, DWORD slot, int* variation = nullptr) {
    __try {
        void* inv = *(void**)((BYTE*)creature + 0xA2C);
        if (!inv) return -1;
        void* item = GetItemInSlot(inv, slot);
        if (!item) return -1;
        if (variation) *variation = *((BYTE*)item + 0x264);  // ModelVariation (item save code at 0x55CDCC)
        return (int)*(DWORD*)((BYTE*)item + 0x0C);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}

// appearance = [[creature + 0xA74] (stats) + 0x186] (stats save code 0x5B21D9); body armor = inventory slot 0x2,
// BodyVariation [item + 0x265] (1 = a), TextureVar [item + 0x266] (item save code 0x55CDFB)
void ReadBody(void* creature, int* appearance, int* letter, int* texVar) {
    *appearance = -1;
    *letter = 0;  // no armor: underwear body 'a'
    *texVar = 1;
    __try {
        BYTE* stats = *(BYTE**)((BYTE*)creature + 0xA74);
        if (stats) *appearance = *(WORD*)(stats + 0x186);
        void* inv = *(void**)((BYTE*)creature + 0xA2C);
        void* armor = inv ? GetItemInSlot(inv, 0x2) : nullptr;
        if (armor) {
            int bv = *((BYTE*)armor + 0x265);
            *letter = bv >= 1 && bv <= 10 ? bv - 1 : 0;
            *texVar = *((BYTE*)armor + 0x266);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void* FindPlayer() {
    __try {
        BYTE* app = *(BYTE**)0x7A39FC;
        if (!app) { static bool t; if (!t) { t = true; Log("weapons: app global is null"); } return nullptr; }
        BYTE* server = *(BYTE**)(app + 8);
        if (!server) return nullptr;
        BYTE* internal = *(BYTE**)(server + 4);
        if (!internal) return nullptr;
        // the PC: first entry of the server player list -> player object -> [player + 0x38] = PC object id
        BYTE* list = *(BYTE**)(internal + 0x10064);
        if (!list) return nullptr;
        void* inner = *(void**)list;
        if (!inner) return nullptr;
        void* pos = *(void**)inner;
        if (!pos) return nullptr;
        void* data = ListGetAt(inner, pos);
        if (!data) return nullptr;
        typedef void*(__thiscall * Cast_t)(void*);
        void* player = ((Cast_t)((*(void***)data)[1]))(data);
        if (!player) return nullptr;
        // [player + 0x34] = the creature being controlled (a party NPC after Tab; 0x7FFFFFFF the PC);
        // [player + 0x38] = the PC. The party table's leader field (+0xEC) holds an NPC index, not an object id.
        DWORD pc = *(DWORD*)((BYTE*)player + 0x34);
        if (!pc || pc == 0x7F000000) pc = *(DWORD*)((BYTE*)player + 0x38);
        void* c = GetCreature(server, pc);
        static DWORD lastId;
        if (pc != lastId) {
            lastId = pc;
            Log("weapons: controlled creature via player list: player %p id %08lX -> creature %p", player, pc, c);
        }
        return c;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

// ---------------------------------------------------------------- drawing
void(APIENTRY* p_glBegin)(GLenum);
void(APIENTRY* p_glEnd)();
void(APIENTRY* p_glVertex3f)(GLfloat, GLfloat, GLfloat);
void(APIENTRY* p_glColor4f)(GLfloat, GLfloat, GLfloat, GLfloat);
void(APIENTRY* p_glLoadMatrixf)(const GLfloat*);
void(APIENTRY* p_glMatrixMode)(GLenum);
void(APIENTRY* p_glPushMatrix)();
void(APIENTRY* p_glPopMatrix)();
void(APIENTRY* p_glPushAttrib)(GLbitfield);
void(APIENTRY* p_glPopAttrib)();
void(APIENTRY* p_glDisable)(GLenum);
void(APIENTRY* p_glEnable)(GLenum);
void(APIENTRY* p_glBlendFunc)(GLenum, GLenum);
void(APIENTRY* p_glDepthMask)(GLboolean);
void(APIENTRY* p_glDepthFunc)(GLenum);
void(APIENTRY* p_glColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
void(APIENTRY* p_glUseProgramObject)(GLuint);  // unused placeholder
void(APIENTRY* p_glActiveTextureARB)(GLenum);
void(APIENTRY* p_glLineWidth)(GLfloat);

bool LoadGl() {
    if (p_glBegin) return true;
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) return false;
#define L(n) p_##n = (decltype(p_##n))GetProcAddress(gl, #n)
    L(glBegin); L(glEnd); L(glVertex3f); L(glColor4f); L(glLoadMatrixf); L(glMatrixMode); L(glPushMatrix);
    L(glPopMatrix); L(glPushAttrib); L(glPopAttrib); L(glDisable); L(glEnable); L(glBlendFunc); L(glDepthMask);
    L(glDepthFunc); L(glColorMask); L(glLineWidth);
#undef L
    typedef PROC(WINAPI * GPA)(LPCSTR);
    GPA wgpa = (GPA)GetProcAddress(gl, "wglGetProcAddress");
    if (wgpa) p_glActiveTextureARB = (decltype(p_glActiveTextureARB))wgpa("glActiveTextureARB");
    return p_glBegin != nullptr;
}

// 8-sided prism along local Z from z0 to z1, centred on (cx, cy), radii rx / ry
void Prism(float cx, float cy, float rx, float ry, float z0, float z1) {
    const int n = 8;
    p_glBegin(GL_QUADS);
    for (int i = 0; i < n; i++) {
        float a0 = i * 6.2831853f / n, a1 = (i + 1) * 6.2831853f / n;
        float x0 = cx + rx * cosf(a0), y0 = cy + ry * sinf(a0), x1 = cx + rx * cosf(a1), y1 = cy + ry * sinf(a1);
        p_glVertex3f(x0, y0, z0); p_glVertex3f(x1, y1, z0); p_glVertex3f(x1, y1, z1); p_glVertex3f(x0, y0, z1);
    }
    p_glEnd();
    // caps
    for (int cap = 0; cap < 2; cap++) {
        float z = cap ? z1 : z0;
        p_glBegin(GL_POLYGON);
        for (int i = 0; i < n; i++) {
            float a = i * 6.2831853f / n;
            p_glVertex3f(cx + rx * cosf(a), cy + ry * sinf(a), z);
        }
        p_glEnd();
    }
}

void Box(float x0, float x1, float y0, float y1, float z0, float z1) {
    p_glBegin(GL_QUADS);
    p_glVertex3f(x0, y0, z0); p_glVertex3f(x1, y0, z0); p_glVertex3f(x1, y1, z0); p_glVertex3f(x0, y1, z0);
    p_glVertex3f(x0, y0, z1); p_glVertex3f(x0, y1, z1); p_glVertex3f(x1, y1, z1); p_glVertex3f(x1, y0, z1);
    p_glVertex3f(x0, y0, z0); p_glVertex3f(x0, y0, z1); p_glVertex3f(x1, y0, z1); p_glVertex3f(x1, y0, z0);
    p_glVertex3f(x0, y1, z0); p_glVertex3f(x1, y1, z0); p_glVertex3f(x1, y1, z1); p_glVertex3f(x0, y1, z1);
    p_glVertex3f(x0, y0, z0); p_glVertex3f(x0, y1, z0); p_glVertex3f(x0, y1, z1); p_glVertex3f(x0, y0, z1);
    p_glVertex3f(x1, y0, z0); p_glVertex3f(x1, y0, z1); p_glVertex3f(x1, y1, z1); p_glVertex3f(x1, y1, z0);
    p_glEnd();
}

void Solid(float r, float g, float b) {
    p_glDisable(GL_BLEND);
    p_glDepthMask(GL_TRUE);
    p_glColor4f(r, g, b, 1);
}
void Glow(float r, float g, float b, float a) {
    p_glEnable(GL_BLEND);
    p_glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    p_glDepthMask(GL_FALSE);
    p_glColor4f(r, g, b, a);
}

// One saber blade from z0 to z1 (z1 further from the hand)
void SaberBlade(float z0, float z1) {
    const float c[3] = {0.25f, 0.55f, 1.0f};  // default blue until the crystal is read
    Solid(0.95f, 0.97f, 1.0f);
    Prism(0, 0, 0.007f, 0.007f, z0, z1);
    Glow(c[0], c[1], c[2], 0.55f);
    Prism(0, 0, 0.016f, 0.016f, z0, z1 - 0.01f * (z1 < z0 ? 1 : -1));
    Glow(c[0], c[1], c[2], 0.22f);
    Prism(0, 0, 0.032f, 0.032f, z0, z1);
}

// Geometry per class in the hand's grip frame (-Z through the fist, +Y up the back of the hand).
void DrawClass(WeaponClass c) {
    switch (c) {
    case WC_SABER:
    case WC_SABER_SHORT: {
        float len = c == WC_SABER_SHORT ? 0.6f : 1.0f;
        Solid(0.25f, 0.25f, 0.27f);
        Prism(0, 0, 0.018f, 0.018f, 0.07f, -0.20f);  // hilt
        Solid(0.55f, 0.55f, 0.6f);
        Prism(0, 0, 0.021f, 0.021f, -0.17f, -0.21f);  // emitter
        SaberBlade(-0.21f, -0.21f - len);
        break;
    }
    case WC_SABER_DOUBLE:
        Solid(0.25f, 0.25f, 0.27f);
        Prism(0, 0, 0.018f, 0.018f, 0.25f, -0.25f);
        SaberBlade(-0.25f, -1.20f);
        SaberBlade(0.25f, 1.20f);
        break;
    case WC_BLADE:
    case WC_BLADE_SHORT: {
        float len = c == WC_BLADE_SHORT ? 0.5f : 0.85f;
        Solid(0.35f, 0.25f, 0.18f);
        Prism(0, 0, 0.016f, 0.016f, 0.08f, -0.12f);  // grip
        Solid(0.45f, 0.45f, 0.48f);
        Box(-0.06f, 0.06f, -0.012f, 0.012f, -0.12f, -0.14f);  // guard
        Solid(0.70f, 0.72f, 0.78f);
        Box(-0.022f, 0.022f, -0.004f, 0.004f, -0.14f, -0.14f - len);
        break;
    }
    case WC_BLADE_DOUBLE:
        Solid(0.35f, 0.25f, 0.18f);
        Prism(0, 0, 0.016f, 0.016f, 0.3f, -0.3f);
        Solid(0.70f, 0.72f, 0.78f);
        Box(-0.022f, 0.022f, -0.004f, 0.004f, -0.3f, -1.1f);
        Box(-0.022f, 0.022f, -0.004f, 0.004f, 0.3f, 1.1f);
        break;
    case WC_BATON:
        Solid(0.18f, 0.18f, 0.2f);
        Prism(0, 0, 0.015f, 0.015f, 0.08f, -0.50f);
        Glow(0.4f, 0.7f, 1.0f, 0.7f);
        Prism(0, 0, 0.022f, 0.022f, -0.48f, -0.56f);
        break;
    case WC_STAFF:
        Solid(0.42f, 0.30f, 0.18f);
        Prism(0, 0, 0.02f, 0.02f, 0.9f, -0.9f);
        break;
    case WC_PISTOL:
        Solid(0.2f, 0.2f, 0.22f);
        Box(-0.018f, 0.018f, -0.11f, 0.0f, 0.04f, -0.02f);   // handle (down from the fist)
        Box(-0.022f, 0.022f, 0.0f, 0.05f, 0.06f, -0.20f);    // body
        Solid(0.1f, 0.1f, 0.1f);
        Prism(0, 0.03f, 0.009f, 0.009f, -0.20f, -0.27f);      // barrel
        break;
    case WC_RIFLE:
    case WC_REPEATER: {
        bool heavy = c == WC_REPEATER;
        Solid(0.22f, 0.22f, 0.24f);
        Box(-0.016f, 0.016f, -0.11f, 0.0f, 0.04f, -0.02f);    // handle
        Box(-0.025f, 0.025f, 0.0f, heavy ? 0.08f : 0.055f, 0.30f, -0.45f);  // receiver + stock
        Solid(0.1f, 0.1f, 0.1f);
        Prism(0, 0.035f, heavy ? 0.02f : 0.011f, heavy ? 0.02f : 0.011f, -0.45f, heavy ? -0.70f : -0.62f);
        break;
    }
    default:
        break;  // unarmed: the hands themselves come later
    }
}

void MulMat(const float a[16], const float b[16], float out[16]) {
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = v;
        }
}

void PoseMatrix(const float pos[3], const float q[4], float m[16]) {  // q = x,y,z,w
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[4] = 2 * (x * y - z * w);     m[8] = 2 * (x * z + y * w);      m[12] = pos[0];
    m[1] = 2 * (x * y + z * w);     m[5] = 1 - 2 * (x * x + z * z); m[9] = 2 * (y * z - x * w);      m[13] = pos[1];
    m[2] = 2 * (x * z - y * w);     m[6] = 2 * (y * z + x * w);     m[10] = 1 - 2 * (x * x + y * y); m[14] = pos[2];
    m[3] = 0; m[7] = 0; m[11] = 0; m[15] = 1;
}

// Two-handed melee held across both hands: axis from the left to the right hand, centred between them.
bool TwoHandMatrix(const KvrInput& in, float m[16]) {
    if ((in.gripValid & 3) != 3) return false;
    const float* l = in.grip[0].pos;
    const float* r = in.grip[1].pos;
    float d[3] = {r[0] - l[0], r[1] - l[1], r[2] - l[2]};
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 0.12f || len > 0.9f) return false;  // hands apart: fall back to the right hand alone
    float z[3] = {-d[0] / len, -d[1] / len, -d[2] / len};  // weapon -Z points toward the right hand
    float up[3] = {0, 1, 0};
    float x[3] = {up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0]};
    float xl = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (xl < 1e-3f) return false;
    for (float& v : x) v /= xl;
    float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    m[0] = x[0]; m[1] = x[1]; m[2] = x[2]; m[3] = 0;
    m[4] = y[0]; m[5] = y[1]; m[6] = y[2]; m[7] = 0;
    m[8] = z[0]; m[9] = z[1]; m[10] = z[2]; m[11] = 0;
    m[12] = (l[0] + r[0]) / 2; m[13] = (l[1] + r[1]) / 2; m[14] = (l[2] + r[2]) / 2; m[15] = 1;
    return true;
}

}  // namespace

const WeaponProfile& Weapon_Profile(WeaponClass c) { return kProfiles[c < WC_COUNT ? c : WC_NONE]; }
WeaponClass Weapon_Right() { return g_right; }
WeaponClass Weapon_Left() { return g_left; }
void* Weapon_PlayerCreature() { return g_player; }

// Combat round object at [creature + 0x9C8]; RoundStarted byte at +0x934, RoundLength (ms) at +0x94C
// (offsets from the round's save code at 0x4D3EC0).
bool Weapon_PlayerInCombat() {
    if (!g_player) return false;
    __try {
        BYTE* round = *(BYTE**)((BYTE*)g_player + 0x9C8);
        if (!round) return false;
        bool started = *(round + 0x934) != 0;
        static bool logged;
        if (started && !logged) {
            logged = true;
            Log("weapons: combat round active, RoundLength %ld, Timer %ld", *(long*)(round + 0x94C), *(long*)(round + 0x944));
        }
        // RoundStarted only goes up once the player attacks; until then, any attack roll in the last 6 s (enemies on
        // the party) counts as combat too, otherwise the first swing could never be sent
        return started || Combat_AttackWithin(6000);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

void Weapons_Update() {
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 250) return;
    last = now;
    void* p = FindPlayer();
    int rv = 1, lv = 1;
    int r = p ? RowInSlot(p, 0x10, &rv) : -1, l = p ? RowInSlot(p, 0x20, &lv) : -1;
    if (Features().weaponGallery && p) {
        // debug: one weapon of every class, 3 s each (row, variation): saber, short saber, double saber, sword,
        // vibroblade, double blade, baton, staff, pistol, heavy pistol, rifle, bowcaster, repeater, heavy repeater
        static const int gallery[][2] = {{8, 1}, {10, 1}, {9, 1}, {2, 1}, {5, 1}, {6, 1}, {1, 1}, {0, 1},
                                         {12, 1}, {13, 1}, {77, 1}, {19, 1}, {23, 1}, {24, 1}};
        int n = (int)(sizeof gallery / sizeof gallery[0]);
        int i = (int)((GetTickCount() / 3000) % n);
        r = gallery[i][0];
        rv = gallery[i][1];
        l = -1;
    }
    if (p != g_player || r != g_rightRow || l != g_leftRow || rv != g_rightVar || lv != g_leftVar) {
        g_player = p;
        g_rightRow = r;
        g_leftRow = l;
        g_rightVar = rv;
        g_leftVar = lv;
        g_right = r >= 0 ? ClassOfBaseItem(r) : WC_NONE;
        g_left = l >= 0 ? ClassOfBaseItem(l) : WC_NONE;
        Log("weapons: player %p right row %d var %d (%s) left row %d var %d (%s)", p, r, rv, kProfiles[g_right].name, l, lv,
            kProfiles[g_left].name);
    }
    // body for the hands (changes with the controlled character and their armor)
    if (p) {
        int app, letter, tv;
        ReadBody(p, &app, &letter, &tv);
        if (app != g_appearance || letter != g_bodyLetter || tv != g_bodyTexVar) {
            g_appearance = app;
            g_bodyLetter = letter;
            g_bodyTexVar = tv;
            g_handModel.clear();
            g_handTexture.clear();
            WeaponModels_BodyFor(app, letter, tv, &g_handModel, &g_handTexture);
            Log("weapons: body appearance %d letter %c texvar %d -> hands %s / %s", app, 'a' + letter, tv,
                g_handModel.c_str(), g_handTexture.c_str());
        }
        // the model cull's picture of this character: body, head (by appearance), held weapons
        char wr[64] = "", wl[64] = "";
        if (g_rightRow >= 0) WeaponModels_ResRef(g_rightRow, g_rightVar, wr, sizeof wr);
        if (g_leftRow >= 0) WeaponModels_ResRef(g_leftRow, g_leftVar, wl, sizeof wl);
        const char* held[2] = {wr, wl};
        OwnSig_SetCharacter(g_appearance, g_handModel.c_str(), held, 2);
    }
}

// ---------------------------------------------------------------- blaster bolts
// The game's own bolt leaves the (hidden) body's hand; ours leaves the muzzle you are holding.
struct Bolt {
    double t0;          // GetTickCount seconds the bolt leaves the muzzle
    float to[3];        // world target
    float from[3];      // world muzzle, set on the first frame after t0
    bool fromSet;
    bool alongBarrel;   // fired by the trigger: flies 30 m down the barrel instead of at a target
};
static double g_lastBarrelShot = -10;
static std::vector<Bolt> g_bolts;
static float g_muzzleEye[16];   // this frame's right-hand ranged weapon matrix in eye space (tip along -Z)
static float g_muzzleLen;
static bool g_muzzleValid;
static float g_lastInvEye[16];
static double g_lastFlash = -10;

static double NowS() { return GetTickCount() / 1000.0; }

void Weapons_FireAlongBarrel() {
    Bolt b{};
    b.t0 = NowS();
    b.alongBarrel = true;
    g_bolts.push_back(b);
    if (g_bolts.size() > 8) g_bolts.erase(g_bolts.begin());
    g_lastBarrelShot = b.t0;
}

void Weapons_FireBolt(const float targetWorld[3], float delay) {
    if (NowS() - g_lastBarrelShot < 0.8) return;  // the trigger pull already showed this shot
    Bolt b{};
    b.t0 = NowS() + delay;
    memcpy(b.to, targetWorld, sizeof b.to);
    g_bolts.push_back(b);
    if (g_bolts.size() > 8) g_bolts.erase(g_bolts.begin());
}

bool Weapon_PlayerPos(float out[3]) {
    void* p = Weapon_PlayerCreature();
    if (!p) return false;
    __try {
        memcpy(out, (BYTE*)p + 0x90, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

void* Weapon_CreatureById(DWORD id) {
    __try {
        BYTE* app = *(BYTE**)0x7A39FC;
        BYTE* server = app ? *(BYTE**)(app + 8) : nullptr;
        return server ? GetCreature(server, id) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

static bool IsSaber(WeaponClass c);

static bool CreaturePos(DWORD id, float out[3]) {
    __try {
        BYTE* c = (BYTE*)Weapon_CreatureById(id);
        if (!c) return false;
        memcpy(out, c + 0x90, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static void ToEye(const float* worldView, const float* p, float* e) {
    for (int r = 0; r < 3; r++)
        e[r] = worldView[0 * 4 + r] * p[0] + worldView[1 * 4 + r] * p[1] + worldView[2 * 4 + r] * p[2] + worldView[12 + r];
}

// Telegraph for every attack rolled against the player, from the roll to just after the impact (v2 Phase A):
// - a downward chevron over the attacker's head: blue = block / parry it, red = dodge it (a bolt you can't deflect);
//   white-gold and larger in the parry window (last 0.4 s);
// - a timing ring around the chevron that closes onto it at the impact;
// - a strike-zone diamond in front of the player's chest on the attacker's side: where the blow arrives - put the
//   blade there (Until You Fall's block line).
// Drawn in eye space so they always face the player, through walls (Arkham's counter icons).
void Weapons_DrawWarnings(const float worldView[16]) {
    if (!Features().cues || !LoadGl()) return;
    CombatWarning w[8];
    int n = Combat_IncomingWarnings(w, 8);
    // bolts: fix each one's world muzzle the first frame it flies, then draw it in this eye
    double tnow = NowS();
    for (Bolt& b : g_bolts) {
        if (b.fromSet || tnow < b.t0 || !g_muzzleValid) continue;
        // muzzle in eye space -> world: inverse of the (rigid) world view
        float me[3] = {g_muzzleEye[8] * -g_muzzleLen + g_muzzleEye[12], g_muzzleEye[9] * -g_muzzleLen + g_muzzleEye[13],
                       g_muzzleEye[10] * -g_muzzleLen + g_muzzleEye[14]};
        float d[3] = {me[0] - worldView[12], me[1] - worldView[13], me[2] - worldView[14]};
        for (int r = 0; r < 3; r++) b.from[r] = worldView[r * 4 + 0] * d[0] + worldView[r * 4 + 1] * d[1] + worldView[r * 4 + 2] * d[2];
        if (b.alongBarrel) {  // the barrel (-Z of the weapon) in eye space -> world, 30 m out
            float de[3] = {-g_muzzleEye[8], -g_muzzleEye[9], -g_muzzleEye[10]};
            for (int r = 0; r < 3; r++) {
                float dw = worldView[r * 4 + 0] * de[0] + worldView[r * 4 + 1] * de[1] + worldView[r * 4 + 2] * de[2];
                b.to[r] = b.from[r] + dw * 30.0f;
            }
        }
        b.fromSet = true;
        g_lastFlash = tnow;
        VR_Haptic(1, 0.9f, 0.06f, 140);  // recoil kick
    }
    bool anyBolt = false;
    for (const Bolt& b : g_bolts)
        if (b.fromSet && tnow - b.t0 < 1.5) anyBolt = true;
    g_bolts.erase(std::remove_if(g_bolts.begin(), g_bolts.end(), [&](const Bolt& b) { return tnow - b.t0 > 1.5; }),
                  g_bolts.end());
    if (n == 0 && !anyBolt) return;
    p_glPushAttrib(GL_ALL_ATTRIB_BITS);
    p_glMatrixMode(GL_MODELVIEW);
    p_glPushMatrix();
    float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    p_glLoadMatrixf(identity);
    p_glDisable(GL_LIGHTING);
    p_glDisable(GL_FOG);
    p_glDisable(GL_ALPHA_TEST);
    p_glDisable(GL_STENCIL_TEST);
    p_glDisable(GL_CULL_FACE);
    p_glDisable(GL_DEPTH_TEST);  // seen through anything, like Arkham's counter icons
    p_glDisable(0x8620 /* GL_VERTEX_PROGRAM_ARB */);
    p_glDisable(0x8804 /* GL_FRAGMENT_PROGRAM_ARB */);
    p_glDisable(0x8870 /* GL_REGISTER_COMBINERS_NV */);
    if (p_glActiveTextureARB) {
        for (int u = 3; u >= 0; u--) {
            p_glActiveTextureARB(0x84C0 + u);
            p_glDisable(GL_TEXTURE_2D);
        }
    } else {
        p_glDisable(GL_TEXTURE_2D);
    }
    p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    p_glEnable(GL_BLEND);
    p_glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    p_glDepthMask(GL_FALSE);
    for (const Bolt& b : g_bolts) {  // a 1 m streak flying at 40 m/s from the muzzle to the target
        if (!b.fromSet) continue;
        float dir[3] = {b.to[0] - b.from[0], b.to[1] - b.from[1], b.to[2] - b.from[2]};
        float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]) + 1e-4f;
        float head = (float)(tnow - b.t0) * 40.0f, tail = head - 1.0f;
        if (tail > len) continue;
        head = min(head, len);
        tail = max(tail, 0.0f);
        float pw[2][3], pe[2][3];
        for (int k = 0; k < 3; k++) {
            pw[0][k] = b.from[k] + dir[k] / len * tail;
            pw[1][k] = b.from[k] + dir[k] / len * head;
        }
        for (int e = 0; e < 2; e++)
            for (int r = 0; r < 3; r++)
                pe[e][r] = worldView[0 * 4 + r] * pw[e][0] + worldView[1 * 4 + r] * pw[e][1] + worldView[2 * 4 + r] * pw[e][2] + worldView[12 + r];
        // a camera-facing ribbon: offset perpendicular to the streak in eye space
        float sx = pe[1][0] - pe[0][0], sy = pe[1][1] - pe[0][1];
        float sl = sqrtf(sx * sx + sy * sy) + 1e-5f;
        float ox = -sy / sl * 0.018f, oy = sx / sl * 0.018f;
        for (int pass = 0; pass < 2; pass++) {
            float k = pass ? 1.0f : 3.0f, a = pass ? 1.0f : 0.25f;
            p_glColor4f(1.0f, pass ? 0.75f : 0.25f, pass ? 0.65f : 0.15f, a);
            p_glBegin(GL_TRIANGLES);
            p_glVertex3f(pe[0][0] - ox * k, pe[0][1] - oy * k, pe[0][2]);
            p_glVertex3f(pe[1][0] - ox * k, pe[1][1] - oy * k, pe[1][2]);
            p_glVertex3f(pe[1][0] + ox * k, pe[1][1] + oy * k, pe[1][2]);
            p_glVertex3f(pe[0][0] - ox * k, pe[0][1] - oy * k, pe[0][2]);
            p_glVertex3f(pe[1][0] + ox * k, pe[1][1] + oy * k, pe[1][2]);
            p_glVertex3f(pe[0][0] + ox * k, pe[0][1] + oy * k, pe[0][2]);
            p_glEnd();
        }
    }
    float pp[3] = {0, 0, 0};
    bool havePlayer = false;
    __try {
        if (BYTE* pc = (BYTE*)Weapon_PlayerCreature()) {
            memcpy(pp, pc + 0x90, sizeof pp);
            havePlayer = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    bool melee = Weapon_Right() != WC_NONE && kProfiles[Weapon_Right()].melee;
    bool saber = IsSaber(Weapon_Right()) || IsSaber(Weapon_Left());
    for (int i = 0; i < n; i++) {
        float ap[3], wp[3];
        bool found = CreaturePos(w[i].attacker, ap);
        static unsigned long lastLogged;
        if (w[i].attacker != lastLogged) {
            Log("telegraph: attack by %08lX, %.2f of %.2f s left, attacker %s", w[i].attacker, w[i].secondsLeft,
                w[i].total, found ? "found" : "NOT found");
            lastLogged = w[i].attacker;
        }
        if (!found) continue;
        float left = w[i].secondsLeft;
        bool parry = left <= 0.4f;
        float fade = left < 0 ? 1.0f + left / 0.15f : 1.0f;  // gone 0.15 s after the impact
        float frac = w[i].total > 0.05f ? max(0.0f, min(1.0f, left / w[i].total)) : 0.0f;  // 1 at the roll, 0 at impact
        bool blockable = w[i].ranged ? saber : melee;
        float base[3] = {blockable ? 0.3f : 1.0f, blockable ? 0.65f : 0.2f, blockable ? 1.0f : 0.15f};
        float rgb[3] = {parry ? 1.0f : base[0], parry ? 0.92f : base[1], parry ? 0.6f : base[2]};
        float e[3];
        // strike zone: chest height, 0.55 m in front of the player towards the attacker
        if (havePlayer) {
            float dx = ap[0] - pp[0], dy = ap[1] - pp[1];
            float dl = sqrtf(dx * dx + dy * dy);
            if (dl > 0.3f) {
                float zp[3] = {pp[0] + dx / dl * 0.55f, pp[1] + dy / dl * 0.55f, pp[2] + 1.2f};
                ToEye(worldView, zp, e);
                if (e[2] < -0.2f) {
                    float a = (0.25f + 0.75f * (1.0f - frac)) * fade, z = 0.06f + 0.04f * (1.0f - frac);
                    if (p_glLineWidth) p_glLineWidth(3.0f);
                    p_glColor4f(rgb[0], rgb[1], rgb[2], a);
                    p_glBegin(GL_LINE_LOOP);
                    p_glVertex3f(e[0], e[1] + z, e[2]);
                    p_glVertex3f(e[0] + z, e[1], e[2]);
                    p_glVertex3f(e[0], e[1] - z, e[2]);
                    p_glVertex3f(e[0] - z, e[1], e[2]);
                    p_glEnd();
                    if (parry) {  // filled in the parry window
                        p_glColor4f(rgb[0], rgb[1], rgb[2], 0.35f * fade);
                        p_glBegin(GL_TRIANGLES);
                        p_glVertex3f(e[0], e[1] + z, e[2]);
                        p_glVertex3f(e[0] + z, e[1], e[2]);
                        p_glVertex3f(e[0], e[1] - z, e[2]);
                        p_glVertex3f(e[0], e[1] + z, e[2]);
                        p_glVertex3f(e[0], e[1] - z, e[2]);
                        p_glVertex3f(e[0] - z, e[1], e[2]);
                        p_glEnd();
                    }
                }
            }
        }
        wp[0] = ap[0];
        wp[1] = ap[1];
        wp[2] = ap[2] + 2.1f;  // above the head (creature origin is at its feet)
        ToEye(worldView, wp, e);
        if (e[2] > -0.3f) continue;  // behind / at the eye
        float s = parry ? 0.22f : 0.15f;
        if (left > 0) {  // timing ring: closes from 3.5x onto the chevron at the impact
            float rr = s * (1.0f + 2.5f * frac);
            if (p_glLineWidth) p_glLineWidth(3.0f);
            p_glColor4f(rgb[0], rgb[1], rgb[2], 0.7f * fade);
            p_glBegin(GL_LINE_LOOP);
            for (int k = 0; k < 28; k++) {
                float an = k * (6.2831853f / 28);
                p_glVertex3f(e[0] + rr * cosf(an), e[1] + rr * sinf(an), e[2]);
            }
            p_glEnd();
        }
        for (int pass = 0; pass < 2; pass++) {  // soft halo, then the core
            float k = pass ? 1.0f : 1.8f, a = (pass ? 0.95f : 0.25f) * fade;
            p_glColor4f(rgb[0], rgb[1], rgb[2], a);
            p_glBegin(GL_TRIANGLES);
            // chevron: outer down-pointing triangle minus a notch, as two triangles
            p_glVertex3f(e[0] - s * k, e[1] + s * 0.6f * k, e[2]);
            p_glVertex3f(e[0], e[1] - s * 0.6f * k, e[2]);
            p_glVertex3f(e[0], e[1] - s * 0.1f * k, e[2]);
            p_glVertex3f(e[0] + s * k, e[1] + s * 0.6f * k, e[2]);
            p_glVertex3f(e[0], e[1] - s * 0.1f * k, e[2]);
            p_glVertex3f(e[0], e[1] - s * 0.6f * k, e[2]);
            p_glEnd();
        }
    }
    p_glPopMatrix();
    p_glPopAttrib();
}

// Spark burst (blocks / hits): bright streaks flying out from a point on the weapon, in the hand's frame.
static void DrawSparks(float strength, double age, float along, const float rgb[3]) {
    p_glDisable(GL_TEXTURE_2D);
    p_glEnable(GL_BLEND);
    p_glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    p_glDepthMask(GL_FALSE);
    if (p_glLineWidth) p_glLineWidth(4.0f);
    unsigned seed = 12345u;
    auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return ((seed >> 8) & 0xFFFF) / 65535.0f * 2 - 1; };
    // a burst of streaks flying out fast (decelerating) plus a bright core flash for the first few frames
    float a = (float)age;
    float r0 = 0.02f + a * 1.6f - a * a * 1.2f, r1 = r0 + 0.08f + a * 0.5f;
    p_glBegin(GL_LINES);
    for (int i = 0; i < 56; i++) {
        float d[3] = {rnd(), rnd(), rnd() * 0.6f};
        float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-4f;
        for (float& v : d) v /= n;
        float sp = 0.6f + 0.4f * (rnd() * 0.5f + 0.5f);
        p_glColor4f(1.0f, 0.95f, 0.85f, strength);
        p_glVertex3f(d[0] * r0 * sp, d[1] * r0 * sp, along + d[2] * r0 * sp);
        p_glColor4f(rgb[0], rgb[1], rgb[2], strength * 0.25f);
        p_glVertex3f(d[0] * r1 * sp, d[1] * r1 * sp, along + d[2] * r1 * sp);
    }
    p_glEnd();
    if (a < 0.08f) {  // core flash: a star of short bright rays
        float c = 0.09f * (1.0f - a / 0.08f);
        p_glBegin(GL_LINES);
        for (int i = 0; i < 8; i++) {
            float ang = i * 0.785398f;
            p_glColor4f(1, 1, 1, strength);
            p_glVertex3f(0, 0, along);
            p_glColor4f(rgb[0], rgb[1], rgb[2], 0);
            p_glVertex3f(cosf(ang) * c, sinf(ang) * c, along);
        }
        p_glEnd();
    }
    p_glDepthMask(GL_TRUE);
}

// Lightsabers are lit only in combat (KotOR draws them only then too): ignite when a fight starts, extinguish 4 s
// after it ends, with the blade extending / retracting over 0.2 s. While lit, the hum follows the blade tip speed.
static float g_bladeExt;  // 0 = hilt only .. 1 = full blade
static bool g_saberLit;
static double g_lastFight = -100, g_lastSaberT;
static float g_lastTip[3];
static double g_lastTipT = -1;

static bool IsSaber(WeaponClass c) { return c == WC_SABER || c == WC_SABER_SHORT || c == WC_SABER_DOUBLE; }

static void UpdateSaber(const KvrInput& in) {
    double t = GetTickCount() / 1000.0;
    double dt = g_lastSaberT > 0 ? t - g_lastSaberT : 0;
    g_lastSaberT = t;
    bool saber = IsSaber(g_right) || IsSaber(g_left);
    if (Weapon_PlayerInCombat()) g_lastFight = t;
    bool want = saber && (t - g_lastFight < 4.0 || Features().weaponGallery);
    if (want != g_saberLit) {
        g_saberLit = want;
        if (saber) {
            Cues_SaberIgnite(want);
            Log("weapons: saber %s", want ? "ignited" : "extinguished");
        }
    }
    float target = g_saberLit ? 1.0f : 0.0f;
    float step = (float)(dt / 0.2);
    g_bladeExt = g_bladeExt < target ? min(target, g_bladeExt + step) : max(target, g_bladeExt - step);
    // blade tip speed (1 m up the blade from the weapon hand), from the host's pose timestamps
    int h = IsSaber(g_right) ? 1 : 0;
    float speed = 0;
    if (in.gripValid & (1u << h)) {
        const float* p = in.grip[h].pos;
        const float* q = in.grip[h].quat;  // x, y, z, w; blade along -Z
        float fwd[3] = {-(2 * (q[0] * q[2] + q[3] * q[1])), -(2 * (q[1] * q[2] - q[3] * q[0])),
                        -(1 - 2 * (q[0] * q[0] + q[1] * q[1]))};
        float tip[3] = {p[0] + fwd[0], p[1] + fwd[1], p[2] + fwd[2]};
        double tdt = in.time - g_lastTipT;
        if (g_lastTipT > 0 && tdt > 0.001 && tdt < 0.2) {
            float dx = tip[0] - g_lastTip[0], dy = tip[1] - g_lastTip[1], dz = tip[2] - g_lastTip[2];
            speed = sqrtf(dx * dx + dy * dy + dz * dz) / (float)tdt;
        }
        if (tdt > 0.001 || g_lastTipT < 0) {
            memcpy(g_lastTip, tip, sizeof g_lastTip);
            g_lastTipT = in.time;
        }
    }
    Cues_SaberHum(saber && g_bladeExt > 0.5f, speed);
}

// Per-class hand pose corrections from kvr.ini [WeaponPose] (re-read whenever the file changes, so a held weapon can
// be tuned in the headset): key = class name with '_' for spaces, value = "roll pitch yaw forward" in degrees and
// metres, applied in the grip frame (roll about the blade / barrel axis, -Z). E.g. short_blade=180 0 0 0.
struct PoseAdj {
    bool set;
    float m[16];
};
static PoseAdj g_poseAdj[WC_COUNT];

static void RefreshPoseAdj() {
    static DWORD lastCheck;
    static FILETIME lastWrite;
    if (GetTickCount() - lastCheck < 1000) return;
    lastCheck = GetTickCount();
    WIN32_FILE_ATTRIBUTE_DATA fa;
    const char* ini = Features_IniPath();
    if (!ini[0] || !GetFileAttributesExA(ini, GetFileExInfoStandard, &fa)) return;
    if (CompareFileTime(&fa.ftLastWriteTime, &lastWrite) == 0) return;
    lastWrite = fa.ftLastWriteTime;
    for (int c = 0; c < WC_COUNT; c++) {
        char key[32], val[64];
        strcpy_s(key, kProfiles[c].name);
        for (char* p = key; *p; p++)
            if (*p == ' ') *p = '_';
        GetPrivateProfileStringA("WeaponPose", key, "", val, sizeof val, ini);
        float roll = 0, pitch = 0, yaw = 0, fwd = 0;
        PoseAdj& a = g_poseAdj[c];
        a.set = sscanf_s(val, "%f %f %f %f", &roll, &pitch, &yaw, &fwd) >= 1;
        if (!a.set) continue;
        const float d = 3.14159265f / 180.0f;
        float cr = cosf(roll * d), sr = sinf(roll * d), cp = cosf(pitch * d), sp = sinf(pitch * d), cy = cosf(yaw * d),
              sy = sinf(yaw * d);
        // R = Ry(yaw) * Rx(pitch) * Rz(roll), column-major, translation along -Z (forward)
        float Rz[16] = {cr, sr, 0, 0, -sr, cr, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float Rx[16] = {1, 0, 0, 0, 0, cp, sp, 0, 0, -sp, cp, 0, 0, 0, 0, 1};
        float Ry[16] = {cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, 0, 0, 0, 1};
        float t[16];
        MulMat(Rx, Rz, t);
        MulMat(Ry, t, a.m);
        a.m[14] = -fwd;
        Log("weapons: pose for %s: roll %.0f pitch %.0f yaw %.0f forward %.2f", kProfiles[c].name, roll, pitch, yaw, fwd);
    }
}

void Weapons_Draw(const float invEye[16]) {
    const KvrShared* shm = VR_Shared();
    if (!shm || !LoadGl()) return;
    KvrInput in;
    memcpy(&in, (const void*)&shm->input, sizeof in);  // a torn read only costs one frame of pose
    if (!in.active || !(in.gripValid & 3)) return;
    UpdateSaber(in);
    bool drawHands = Features().hands && !g_handModel.empty();
    if (g_right == WC_NONE && g_left == WC_NONE && !drawHands) return;

    p_glPushAttrib(GL_ALL_ATTRIB_BITS);
    p_glMatrixMode(GL_MODELVIEW);
    p_glPushMatrix();
    p_glDisable(GL_LIGHTING);
    p_glDisable(GL_FOG);
    p_glDisable(GL_ALPHA_TEST);
    p_glDisable(GL_STENCIL_TEST);
    p_glDisable(GL_CULL_FACE);
    p_glDisable(0x8620 /* GL_VERTEX_PROGRAM_ARB */);
    p_glDisable(0x8804 /* GL_FRAGMENT_PROGRAM_ARB */);
    p_glDisable(0x8870 /* GL_REGISTER_COMBINERS_NV */);
    if (p_glActiveTextureARB) {
        for (int u = 3; u >= 0; u--) {
            p_glActiveTextureARB(0x84C0 + u);
            p_glDisable(GL_TEXTURE_2D);
            p_glDisable(0x8513 /* GL_TEXTURE_CUBE_MAP */);
        }
    } else {
        p_glDisable(GL_TEXTURE_2D);
    }
    p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    p_glEnable(GL_DEPTH_TEST);
    p_glDepthFunc(GL_LEQUAL);

    float hand[16], mv[16];
    if (drawHands) {
        // the character's own hands at the controllers: palm center on the grip (the hand frame's origin is the wrist,
        // ~7 cm behind it), fingers curling with grip (index with trigger); a held weapon closes the fist around it
        // tracked hand skeletons from the host (real hands, or the runtime's controller-driven hand pose) when present
        KvrHands hs;
        memcpy(&hs, (const void*)&shm->hands, sizeof hs);  // a torn read only costs one frame of pose
        p_glLoadMatrixf(invEye);  // vertices are skinned straight into the tracking frame
        for (int h = 0; h < 2; h++) {
            bool tracked = (hs.valid & (1u << h)) != 0;
            if (!tracked && !(in.gripValid & (1u << h))) continue;
            WeaponClass held = h ? g_right : g_left;
            bool holding = held != WC_NONE || (h == 0 && kProfiles[g_right].twoHanded);
            float grip = h ? in.gripR : in.gripL, trig = h ? in.trigR : in.trigL;
            float curl = holding ? max(grip, 0.85f) : grip, idx = holding ? max(trig, 0.8f) : trig;
            WeaponModels_DrawHand(g_handModel.c_str(), g_handTexture.c_str(), h, tracked ? hs.joint[h] : nullptr,
                                  &in.grip[h], curl, idx, Cues_ForceGlow(h));
        }
    }
    // Ranged weapons point along the controller's aim ray, not its grip axis (the grip pose is tilted ~40 deg up from
    // where a pointing hand aims): the grip -> aim rotation, measured on the right controller, applied to both hands.
    float gripToAim[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if ((in.gripValid & 2) && fabsf(in.aim.quat[3]) + fabsf(in.aim.quat[0]) + fabsf(in.aim.quat[1]) + fabsf(in.aim.quat[2]) > 0.5f) {
        float G[16], A[16];
        float zero[3] = {0, 0, 0};
        PoseMatrix(zero, in.grip[1].quat, G);
        PoseMatrix(zero, in.aim.quat, A);
        for (int c = 0; c < 3; c++)
            for (int r = 0; r < 3; r++) {
                float v = 0;
                for (int k = 0; k < 3; k++) v += G[r * 4 + k] * A[c * 4 + k];  // G^T * A
                gripToAim[c * 4 + r] = v;
            }
    }
    memcpy(g_lastInvEye, invEye, sizeof g_lastInvEye);
    RefreshPoseAdj();
    g_muzzleValid = false;
    for (int h = 1; h >= 0; h--) {  // right, then left
        WeaponClass c = h ? g_right : g_left;
        if (c == WC_NONE || !(in.gripValid & (1u << h))) continue;
        if (Weapon_Profile(c).twoHanded && kProfiles[c].melee && h == 1 && TwoHandMatrix(in, hand)) {
            // held between both hands
        } else if (!kProfiles[c].melee) {
            float g[16];
            PoseMatrix(in.grip[h].pos, in.grip[h].quat, g);
            MulMat(g, gripToAim, hand);
        } else {
            // Power Attack: the blade's rotation drags behind the hand for a moment (weight); the hilt stays in the hand
            static float lagQ[2][4];
            static double lagT[2] = {-1, -1};
            const float* q = in.grip[h].quat;
            float lag = Cues_PowerLag();
            double ldt = lagT[h] > 0 ? in.time - lagT[h] : 0;
            lagT[h] = in.time;
            if (lag > 0 && ldt > 0 && ldt < 0.2) {
                float dot = lagQ[h][0] * q[0] + lagQ[h][1] * q[1] + lagQ[h][2] * q[2] + lagQ[h][3] * q[3];
                float sgn = dot < 0 ? -1.0f : 1.0f, a = 1.0f - expf(-(float)ldt / lag), len = 0;
                for (int k = 0; k < 4; k++) {
                    lagQ[h][k] += (q[k] * sgn - lagQ[h][k]) * a;
                    len += lagQ[h][k] * lagQ[h][k];
                }
                len = sqrtf(len) + 1e-6f;
                for (int k = 0; k < 4; k++) lagQ[h][k] /= len;
            } else {
                memcpy(lagQ[h], q, sizeof lagQ[h]);
            }
            PoseMatrix(in.grip[h].pos, lagQ[h], hand);
        }
        if (g_poseAdj[c].set) {
            float adjusted[16];
            MulMat(hand, g_poseAdj[c].m, adjusted);
            memcpy(hand, adjusted, sizeof adjusted);
        }
        float yawSign = 1, knock = kProfiles[c].melee ? Cues_KnockOff(&yawSign) : 0;
        if (knock > 0) {  // block knock-off: rotate the weapon about its grip, tip thrown up and to one side
            float ax = 0.8f, ay = 0.6f * yawSign, az = 0;
            float an = knock * 3.14159265f / 180.0f, cs = cosf(an), sn = sinf(an), ic = 1 - cs;
            float R[16] = {cs + ax * ax * ic,      ay * ax * ic + az * sn, az * ax * ic - ay * sn, 0,
                           ax * ay * ic - az * sn, cs + ay * ay * ic,      az * ay * ic + ax * sn, 0,
                           ax * az * ic + ay * sn, ay * az * ic - ax * sn, cs + az * az * ic,      0,
                           0, 0, 0, 1};
            float knocked[16];
            MulMat(hand, R, knocked);
            memcpy(hand, knocked, sizeof knocked);
        }
        MulMat(invEye, hand, mv);
        p_glLoadMatrixf(mv);
        int row = h ? g_rightRow : g_leftRow, var = h ? g_rightVar : g_leftVar;
        float glowRgb[3];
        Cues_GlowColor(h, glowRgb);
        if (!WeaponModels_Draw(row, var, Cues_Glow(h), IsSaber(c) ? g_bladeExt : 1.0f, glowRgb)) DrawClass(c);
        if (!kProfiles[c].melee && h == 1) {
            memcpy(g_muzzleEye, mv, sizeof g_muzzleEye);
            g_muzzleLen = WeaponModels_Length(row, var);
            if (g_muzzleLen < 0.1f) g_muzzleLen = 0.25f;
            g_muzzleValid = true;
            if (NowS() - g_lastFlash < 0.07) {  // muzzle flash: a hot star at the tip
                p_glDisable(GL_TEXTURE_2D);
                p_glEnable(GL_BLEND);
                p_glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                p_glDepthMask(GL_FALSE);
                float z = -g_muzzleLen, r = 0.05f;
                p_glBegin(GL_TRIANGLES);
                for (int k = 0; k < 6; k++) {
                    float a0 = k * 1.0472f, a1 = a0 + 0.35f;
                    p_glColor4f(1.0f, 0.9f, 0.7f, 1.0f);
                    p_glVertex3f(0, 0, z);
                    p_glColor4f(1.0f, 0.4f, 0.2f, 0.0f);
                    p_glVertex3f(r * cosf(a0), r * sinf(a0), z - 0.02f);
                    p_glVertex3f(r * cosf(a1), r * sinf(a1), z - 0.02f);
                }
                p_glEnd();
                p_glDepthMask(GL_TRUE);
            }
        }
        double sparkAge;
        float sparkRgb[3];
        float spark = Cues_Sparks(&sparkAge, sparkRgb);
        if (spark > 0 && h == 1)
            DrawSparks(spark, sparkAge, IsSaber(c) ? -0.55f : kProfiles[c].melee ? -0.35f : -0.2f, sparkRgb);  // the game's own mesh; procedural proxy as fallback
        if (kProfiles[c].twoHanded) break;  // a two-handed weapon occupies both hands
    }

    p_glPopMatrix();
    p_glPopAttrib();
}
