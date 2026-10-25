// Equipped-weapon detection, weapon profiles and in-hand proxies (x86, inside swkotor.exe).
//
// Player and equipment come straight from the server-side game objects (Steam build addresses, see FINDINGS.md):
//   app      = [0x7A39FC];  server = [app + 8];  internal = [server + 4]
//   party    = internal + 0x1B770;  leader id = [party + 0xEC]       (the character the player controls)
//   creature = 0x4AE770(server, id)          thiscall: object id -> server creature
//   item     = 0x5A4C20([creature + 0xA2C], slot)   thiscall: inventory -> item in slot (0x10 right, 0x20 left)
//   base     = [item + 0x0C]                 row in baseitems.2da
// Everything is read under SEH guards and refreshed four times a second.

#include "weapons.h"

#include <GL/gl.h>
#include <cmath>
#include <cstring>

#include "../common/kvr_shared.h"
#include "vr.h"
#include "weaponmodels.h"
#include "features.h"

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

void* FindPlayer() {
    __try {
        BYTE* app = *(BYTE**)0x7A39FC;
        if (!app) { static bool t; if (!t) { t = true; Log("weapons: app global is null"); } return nullptr; }
        BYTE* server = *(BYTE**)(app + 8);
        if (!server) return nullptr;
        BYTE* internal = *(BYTE**)(server + 4);
        if (!internal) return nullptr;
        // party leader when the player switched to a party NPC (-1 = the PC itself leads)
        DWORD leader = *(DWORD*)(internal + 0x1B770 + 0xEC);
        if (leader && leader != 0xFFFFFFFF && leader != 0x7F000000) {
            void* c = GetCreature(server, leader);
            if (c) return c;
        }
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
        DWORD pc = *(DWORD*)((BYTE*)player + 0x38);
        void* c = GetCreature(server, pc);
        static int traces;
        if (traces < 3) {
            traces++;
            Log("weapons: PC via player list: player %p id %08lX -> creature %p (leader %08lX)", player, pc, c, leader);
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

bool LoadGl() {
    if (p_glBegin) return true;
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) return false;
#define L(n) p_##n = (decltype(p_##n))GetProcAddress(gl, #n)
    L(glBegin); L(glEnd); L(glVertex3f); L(glColor4f); L(glLoadMatrixf); L(glMatrixMode); L(glPushMatrix);
    L(glPopMatrix); L(glPushAttrib); L(glPopAttrib); L(glDisable); L(glEnable); L(glBlendFunc); L(glDepthMask);
    L(glDepthFunc); L(glColorMask);
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
        return started;
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
}

void Weapons_Draw(const float invEye[16]) {
    const KvrShared* shm = VR_Shared();
    if (!shm || !LoadGl()) return;
    KvrInput in;
    memcpy(&in, (const void*)&shm->input, sizeof in);  // a torn read only costs one frame of pose
    if (!in.active || !(in.gripValid & 3)) return;
    if (g_right == WC_NONE && g_left == WC_NONE) return;

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
    for (int h = 1; h >= 0; h--) {  // right, then left
        WeaponClass c = h ? g_right : g_left;
        if (c == WC_NONE || !(in.gripValid & (1u << h))) continue;
        if (Weapon_Profile(c).twoHanded && kProfiles[c].melee && h == 1 && TwoHandMatrix(in, hand)) {
            // held between both hands
        } else {
            PoseMatrix(in.grip[h].pos, in.grip[h].quat, hand);
        }
        MulMat(invEye, hand, mv);
        p_glLoadMatrixf(mv);
        int row = h ? g_rightRow : g_leftRow, var = h ? g_rightVar : g_leftVar;
        if (!WeaponModels_Draw(row, var)) DrawClass(c);  // the game's own mesh; procedural proxy as fallback
        if (kProfiles[c].twoHanded) break;  // a two-handed weapon occupies both hands
    }

    p_glPopMatrix();
    p_glPopAttrib();
}
