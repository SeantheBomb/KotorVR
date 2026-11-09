// In-game VR layer (x86, inside swkotor.exe).
//
// Alternate-eye rendering: every frame that draws the 3D world renders one eye. The camera sequence
// the engine emits is fixed (see docs/FINDINGS.md):
//   PROJECTION: glLoadIdentity; gluPerspective(fovy, 4:3, n, f)       -> replaced by the eye frustum
//   MODELVIEW:  glLoadIdentity; glMultMatrixf(R); glTranslatef(-p)     -> R replaced by inv(E) * R
//   glProgramEnvParameter4fARB(VERTEX, 92..89) = rows of [R^T | p]      -> replaced by rows of [R^T|p] * E
// E is the eye pose relative to the recentered head origin, in the game camera's space (both GL/OpenXR:
// +X right, +Y up, -Z forward, meters). Game camera stays the engine's own (orbit) camera for now.
//
// Frame export: async glReadPixels into PBOs -> UpdateSubresource into a keyed-mutex shared D3D11 texture
// -> kvr_host. (NV_DX_interop crashed the driver alongside the game's pbuffer contexts.)

#include "vr.h"
#include <vector>
#include "scan.h"
#include "enginecam.h"
#include "weapons.h"
#include "anim.h"
#include "melee.h"
#include "combat.h"
#include "features.h"
#include "mirror.h"
#include "ownsig.h"

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cstdio>

#include "../common/kvr_shared.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#define GL_VERTEX_PROGRAM_ARB 0x8620
#define GL_PIXEL_PACK_BUFFER_ARB 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING_ARB 0x88ED
#define GL_STREAM_READ_ARB 0x88E1
#define GL_READ_ONLY_ARB 0x88B8

// Shared-texture keyed mutex protocol: game acquires KEY_GAME, writes, releases KEY_HOST;
// host acquires KEY_HOST, copies out, releases KEY_GAME. Neither side touches the texture without the key.
enum { KEY_GAME = 0, KEY_HOST = 1 };

static VrGl g_gl;
static void (*g_log)(const char*, ...);

// GL entry points we call directly
static void(APIENTRY* p_glGetIntegerv)(GLenum, GLint*);
static void(APIENTRY* p_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
static GLenum(APIENTRY* p_glGetError)();
static HGLRC(WINAPI* p_wglGetCurrentContext)();
static void(APIENTRY* p_glGenBuffersARB)(GLsizei, GLuint*);
static void(APIENTRY* p_glDeleteBuffersARB)(GLsizei, const GLuint*);
static void(APIENTRY* p_glBindBufferARB)(GLenum, GLuint);
static void(APIENTRY* p_glBufferDataARB)(GLenum, ptrdiff_t, const void*, GLenum);
static void*(APIENTRY* p_glMapBufferARB)(GLenum, GLenum);
static GLboolean(APIENTRY* p_glUnmapBufferARB)(GLenum);

// shared memory
static KvrShared* g_shm;
static DWORD g_lastShmTry;

// export state: per slot, two PBOs ping-ponged so a readback is mapped one capture after it was issued
enum { SLOT_EYE0, SLOT_EYE1, SLOT_PANEL, SLOT_COUNT };
struct Slot {
    ID3D11Texture2D* shared = nullptr;  // keyed-mutex NT-handle texture opened by the host
    IDXGIKeyedMutex* mutex = nullptr;
    HANDLE sharedHandle = nullptr;
    GLuint pbo[2] = {0, 0};
    int next = 0;  // PBO the next readback goes into
    bool pending[2] = {false, false};
    KvrPose pose[2];  // metadata captured with each PBO
    KvrFov fov[2];
    uint32_t flags[2] = {0, 0};
};
static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static Slot g_slots[SLOT_COUNT];
static int g_texW, g_texH;
static uint32_t g_texGen;
static bool g_exportReady, g_exportFailed;
static HGLRC g_exportCtx;
static bool g_inCapture;
static int g_diagLeft = 2;

// stall watchdog: log any export step slower than 20 ms
static LARGE_INTEGER g_qpf;
static double NowMs() {
    LARGE_INTEGER t;
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / g_qpf.QuadPart;
}
#define TIMED(label, stmt)                                                               do {                                                                                     double t0_ = NowMs();                                                                stmt;                                                                                double dt_ = NowMs() - t0_;                                                          if (dt_ > 20.0) g_log("STALL %s slot %d: %.1f ms", label, slotIndex, dt_);       } while (0)

// per-frame camera state
static bool g_vrActive;        // host is running and export is ready
static bool g_worldThisFrame;  // a world perspective was replaced this frame
static bool g_eyeCaptured;
static int g_camState;         // 0 idle, 1 expect R, 2 expect translate, 3 expect env params
static unsigned g_worldFrames; // parity picks the eye
static int g_curEye;
static KvrPose g_curPose;
static KvrFov g_curFov;
static float g_R[16];          // game camera rotation as passed to glMultMatrixf (column-major)
static float g_Q[9];           // eye rotation (row-major 3x3)
static float g_t[3];           // eye translation
static float g_envRows[3][4];  // replacement for env params 92, 91, 90
static double g_gameFovy = 55.0, g_gameAspect = 4.0 / 3.0;  // the game's own projection (drives its picking)

// ---------------------------------------------------------------- shared memory
static bool EnsureShm() {
    if (g_shm) return true;
    DWORD now = GetTickCount();
    if (now - g_lastShmTry < 1000) return false;
    g_lastShmTry = now;
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(KvrShared), KVR_SHM_NAME);
    if (!h) return false;
    g_shm = (KvrShared*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(KvrShared));
    if (g_shm && (g_shm->magic != KVR_MAGIC || g_shm->version != KVR_VERSION)) {
        ZeroMemory(g_shm, sizeof(KvrShared));
        g_shm->magic = KVR_MAGIC;
        g_shm->version = KVR_VERSION;
    }
    if (g_shm) {  // the host finds the exported GUI icons through the game folder
        char exe[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
        while (n > 0 && exe[n - 1] != '\\') n--;
        if (n > 0) exe[n - 1] = 0;
        strncpy_s(g_shm->gameDir, exe, _TRUNCATE);
    }
    g_log("shared memory %s", g_shm ? "mapped" : "failed");
    return g_shm != nullptr;
}

static bool ReadHost(KvrHostState& out) {
    for (int tries = 0; tries < 100; tries++) {
        uint32_t s1 = g_shm->host.seq;
        if (s1 & 1) continue;
        MemoryBarrier();
        memcpy(&out, (const void*)&g_shm->host, sizeof out);
        MemoryBarrier();
        if (g_shm->host.seq == s1) return true;
    }
    return false;
}

// ---------------------------------------------------------------- export (GL PBO readback -> D3D11 -> host)
static bool InitDevice() {
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    p_glGetIntegerv = (decltype(p_glGetIntegerv))GetProcAddress(gl, "glGetIntegerv");
    p_glReadPixels = (decltype(p_glReadPixels))GetProcAddress(gl, "glReadPixels");
    p_glGetError = (decltype(p_glGetError))GetProcAddress(gl, "glGetError");
    p_wglGetCurrentContext = (decltype(p_wglGetCurrentContext))GetProcAddress(gl, "wglGetCurrentContext");
    p_glGenBuffersARB = (decltype(p_glGenBuffersARB))g_gl.GetProcAddress("glGenBuffersARB");
    p_glDeleteBuffersARB = (decltype(p_glDeleteBuffersARB))g_gl.GetProcAddress("glDeleteBuffersARB");
    p_glBindBufferARB = (decltype(p_glBindBufferARB))g_gl.GetProcAddress("glBindBufferARB");
    p_glBufferDataARB = (decltype(p_glBufferDataARB))g_gl.GetProcAddress("glBufferDataARB");
    p_glMapBufferARB = (decltype(p_glMapBufferARB))g_gl.GetProcAddress("glMapBufferARB");
    p_glUnmapBufferARB = (decltype(p_glUnmapBufferARB))g_gl.GetProcAddress("glUnmapBufferARB");
    if (!p_glGenBuffersARB || !p_glBindBufferARB || !p_glBufferDataARB || !p_glMapBufferARB || !p_glUnmapBufferARB) {
        g_log("export: ARB buffer objects not available");
        return false;
    }
    g_exportCtx = p_wglGetCurrentContext();

    // D3D11 on the NVIDIA adapter (same GPU as the host)
    IDXGIFactory1* fac = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac);
    IDXGIAdapter1* chosen = nullptr;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; fac && fac->EnumAdapters1(i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        if (d.VendorId == 0x10DE && !chosen) { chosen = a; continue; }
        a->Release();
    }
    if (fac) fac->Release();
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                                   D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
    if (chosen) chosen->Release();
    if (FAILED(hr)) { g_log("export: D3D11CreateDevice failed 0x%08X", hr); return false; }
    g_log("export device ready (PBO readback), ctx %p", g_exportCtx);
    return true;
}

static void DestroySlots() {
    bool glOk = p_wglGetCurrentContext && p_wglGetCurrentContext() == g_exportCtx;
    for (auto& s : g_slots) {
        if (s.pbo[0] && glOk) p_glDeleteBuffersARB(2, s.pbo);
        if (s.mutex) s.mutex->Release();
        if (s.shared) s.shared->Release();
        if (s.sharedHandle) CloseHandle(s.sharedHandle);
        s = Slot{};
    }
}

void VR_BeforeShareLists() {}  // PBOs are plain buffer objects; context sharing is harmless now

// (Re)create the export resources at the window's client size. Names carry a generation so the
// host can reopen after a resize while it still holds the previous ones.
static bool CreateSlots(int w, int h) {
    DestroySlots();
    g_texW = w;
    g_texH = h;
    g_texGen = g_texGen ? g_texGen + 1 : ((GetCurrentProcessId() & 0xFFFFF) << 8) + 1;  // unique per launch: the host may still hold a dead game's textures
    const wchar_t* bases[SLOT_COUNT] = {KVR_TEX_NAME_EYE0, KVR_TEX_NAME_EYE1, KVR_TEX_NAME_PANEL};
    for (int i = 0; i < SLOT_COUNT; i++) {
        Slot& s = g_slots[i];
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &s.shared))) { g_log("export: texture creation failed"); return false; }
        s.shared->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&s.mutex);
        IDXGIResource1* res = nullptr;
        s.shared->QueryInterface(__uuidof(IDXGIResource1), (void**)&res);
        wchar_t name[96];
        swprintf_s(name, L"%s_%u", bases[i], g_texGen);
        HRESULT hr = res ? res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, name, &s.sharedHandle) : E_FAIL;
        if (res) res->Release();
        if (FAILED(hr)) { g_log("export: CreateSharedHandle failed 0x%08X", hr); return false; }

        GLint prev = 0;
        p_glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING_ARB, &prev);
        p_glGenBuffersARB(2, s.pbo);
        for (GLuint b : s.pbo) {
            p_glBindBufferARB(GL_PIXEL_PACK_BUFFER_ARB, b);
            p_glBufferDataARB(GL_PIXEL_PACK_BUFFER_ARB, (ptrdiff_t)w * h * 4, nullptr, GL_STREAM_READ_ARB);
        }
        p_glBindBufferARB(GL_PIXEL_PACK_BUFFER_ARB, prev);
    }
    for (auto& e : g_shm->eyes) e.frameId = 0;
    g_shm->panel.frameId = 0;
    g_shm->texWidth = w;
    g_shm->texHeight = h;
    g_shm->texGeneration = g_texGen;
    g_log("export resources %dx%d gen %u", w, h, g_texGen);
    return true;
}

// Deliver a finished readback to the host if it has consumed the previous one.
static void Deliver(int slotIndex, int which) {
    Slot& s = g_slots[slotIndex];
    if (!s.pending[which]) return;
    s.pending[which] = false;
    HRESULT acq;
    TIMED("acquire", acq = s.mutex->AcquireSync(KEY_GAME, 0));
    if (acq != S_OK) return;  // host still holds the last frame; drop this one
    GLint prev = 0;
    p_glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING_ARB, &prev);
    p_glBindBufferARB(GL_PIXEL_PACK_BUFFER_ARB, s.pbo[which]);
    const void* px;
    TIMED("map", px = p_glMapBufferARB(GL_PIXEL_PACK_BUFFER_ARB, GL_READ_ONLY_ARB));
    bool ok = px != nullptr;
    if (ok) {
        TIMED("upload", g_ctx->UpdateSubresource(s.shared, 0, nullptr, px, g_texW * 4, 0));
        if (g_diagLeft > 0 && slotIndex == SLOT_PANEL) {
            g_diagLeft--;
            const unsigned char* c = (const unsigned char*)px + ((g_texH / 2) * g_texW + g_texW / 2) * 4;
            g_log("diag panel center = %u %u %u %u", c[0], c[1], c[2], c[3]);
        }
        p_glUnmapBufferARB(GL_PIXEL_PACK_BUFFER_ARB);
    }
    p_glBindBufferARB(GL_PIXEL_PACK_BUFFER_ARB, prev);
    if (ok) {
        KvrEyeSlot* out = slotIndex == SLOT_PANEL ? &g_shm->panel : &g_shm->eyes[slotIndex];
        out->pose = s.pose[which];
        out->fov = s.fov[which];
        out->width = g_texW;
        out->height = g_texH;
        out->flags = s.flags[which];
        out->frameId++;
    }
    TIMED("release", s.mutex->ReleaseSync(ok ? KEY_HOST : KEY_GAME));
    TIMED("flush", g_ctx->Flush());
}

static void ClearForGui();
static bool g_guiOnlyThisFrame;  // this frame's GUI was drawn over black (panel capture is GUI-only)

// Start an async readback of the current back buffer into a slot; deliver the one issued two captures ago.
static void Capture(int slotIndex, const KvrPose* pose, const KvrFov* fov) {
    Slot& s = g_slots[slotIndex];
    if (!s.shared || !s.pbo[0]) return;
    if (p_wglGetCurrentContext() != g_exportCtx) {
        static int n;
        if (n++ < 5) g_log("capture skipped: current ctx %p != export ctx %p", p_wglGetCurrentContext(), g_exportCtx);
        return;
    }
    g_inCapture = true;
    int w = s.next;
    Deliver(slotIndex, w);
    GLint prev = 0;
    p_glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING_ARB, &prev);
    p_glBindBufferARB(GL_PIXEL_PACK_BUFFER_ARB, s.pbo[w]);
    TIMED("readpixels", p_glReadPixels(0, 0, g_texW, g_texH, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
    p_glBindBufferARB(GL_PIXEL_PACK_BUFFER_ARB, prev);
    s.pending[w] = true;
    if (pose) s.pose[w] = *pose;
    if (fov) s.fov[w] = *fov;
    s.flags[w] = (slotIndex == SLOT_PANEL && g_guiOnlyThisFrame) ? KVR_SLOT_GUI_ONLY : 0;
    s.next = w ^ 1;
    GLenum err = p_glGetError();
    if (err != GL_NO_ERROR) {
        static int n;
        if (n++ < 10) g_log("capture slot %d: glError 0x%x", slotIndex, err);
    }
    g_inCapture = false;
}

// After the eye image is taken, clear to black so the GUI pass draws onto nothing: the panel capture at swap
// is then GUI-only, which the host keys over the stereo world as a HUD.
static void ClearForGui() {
    static void(APIENTRY* p_glClear)(GLbitfield);
    static void(APIENTRY* p_glClearColor)(GLclampf, GLclampf, GLclampf, GLclampf);
    static void(APIENTRY* p_glColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
    static GLboolean(APIENTRY* p_glIsEnabled)(GLenum);
    static void(APIENTRY* p_glDisable)(GLenum);
    static void(APIENTRY* p_glEnable)(GLenum);
    static void(APIENTRY* p_glGetFloatv)(GLenum, GLfloat*);
    static void(APIENTRY* p_glGetBooleanv)(GLenum, GLboolean*);
    if (!p_glClear) {
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        p_glClear = (decltype(p_glClear))GetProcAddress(gl, "glClear");
        p_glClearColor = (decltype(p_glClearColor))GetProcAddress(gl, "glClearColor");
        p_glColorMask = (decltype(p_glColorMask))GetProcAddress(gl, "glColorMask");
        p_glIsEnabled = (decltype(p_glIsEnabled))GetProcAddress(gl, "glIsEnabled");
        p_glDisable = (decltype(p_glDisable))GetProcAddress(gl, "glDisable");
        p_glEnable = (decltype(p_glEnable))GetProcAddress(gl, "glEnable");
        p_glGetFloatv = (decltype(p_glGetFloatv))GetProcAddress(gl, "glGetFloatv");
        p_glGetBooleanv = (decltype(p_glGetBooleanv))GetProcAddress(gl, "glGetBooleanv");
    }
    GLfloat cc[4];
    GLboolean mask[4];
    p_glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    p_glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    GLboolean scissor = p_glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) p_glDisable(GL_SCISSOR_TEST);
    p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    p_glClearColor(0, 0, 0, 0);
    p_glClear(GL_COLOR_BUFFER_BIT);
    p_glClearColor(cc[0], cc[1], cc[2], cc[3]);
    p_glColorMask(mask[0], mask[1], mask[2], mask[3]);
    if (scissor) p_glEnable(GL_SCISSOR_TEST);
    g_guiOnlyThisFrame = true;
}

// ---------------------------------------------------------------- math
static void QuatToRows(const float q[4], float m[9]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w);     m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w);     m[7] = 2 * (y * z + x * w);     m[8] = 1 - 2 * (x * x + y * y);
}

// ---------------------------------------------------------------- per-frame eye choice
// The controlled creature's world position, refreshed every frame (model culling matches against it).
static float g_playerPos[3];
static bool g_playerPosValid;
static float g_nearestOwn = 1e9f;  // diagnostics: closest object origin to the player this frame
static int g_rootDepth = -1;        // matrix stack depth of model roots (where the player's body root matched)
// The character's own body bones (world positions) this frame and last: held weapons and the head are separate
// models whose root sits exactly on a bone (hand, neck). The game draws them before the body, so last frame's bones.
static void NearEyeFrameEnd();
static float g_hooks[8][3];  // the body's attachment points this frame (Anim_PlayerHooks)
static int g_hookCount;
static volatile bool g_traceNow;  // the test harness asked for a trace of the next world frame
static float g_lastRootT[3];  // the latest model root (root depth) seen by VR_IsOwnBody, world
static float g_ownNodes[2][128][3];
static int g_ownNodeCount[2];
static int g_ownNodeCur;

static void RefreshPlayerPos() {
    g_playerPosValid = false;
    // the body model's own world position (Gob +0x78) is exactly where its hierarchy is drawn; the server creature's
    // position leads it while you move, which let the body pop into view (2026-10-27)
    float prev[3] = {g_playerPos[0], g_playerPos[1], g_playerPos[2]};
    bool had = g_playerPosValid;
    if (Anim_PlayerBodyPos(g_playerPos)) {
        g_playerPosValid = true;
        float dx = g_playerPos[0] - prev[0], dy = g_playerPos[1] - prev[1];
        if (had && dx * dx + dy * dy > 0.6f * 0.6f) {
            // you switched to another character (Tab) or were moved: last frame's bones belong to the old body -
            // forget them, so its head shows at once and the new head is hidden by the neck rule this frame
            g_ownNodeCount[0] = g_ownNodeCount[1] = 0;
        }
        return;
    }
    __try {
        BYTE* c = (BYTE*)Weapon_PlayerCreature();
        if (c) {
            memcpy(g_playerPos, c + 0x90, sizeof g_playerPos);
            g_playerPosValid = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static double g_pickUntilMs;  // aim pick active until (NowMs)
static int g_probeState;         // 0 idle, 1 probing this frame, 2 result ready
static float g_probeDist;
static bool g_probeClear;
static float g_probeDepth;
static KvrPose g_pickPose;

static bool g_eyeChosen;
static bool g_inWorldPass;  // between the world perspective and the first ortho (post-processing / GUI)
static double g_zn = 0.1, g_zf = 10000.0;  // the world pass's clip planes (weapon proxies must share its depth)

// The world pass's own view matrix (engine-camera mode): R from its glMultMatrixf, p from the glTranslatef after it.
static float g_engR[16], g_worldView[16];
static bool g_worldViewValid;

// Draw the in-hand weapons into the finished world image (scene depth still valid), in this eye's projection.
static void DrawHandItems() {
    if (!Features().weaponProxies) return;
    static void(APIENTRY* p_glMatrixMode)(GLenum);
    static void(APIENTRY* p_glPushMatrix)();
    static void(APIENTRY* p_glPopMatrix)();
    static void(APIENTRY* p_glLoadIdentity)();
    static void(APIENTRY* p_glViewport)(GLint, GLint, GLsizei, GLsizei);
    static void(APIENTRY* p_glGetIntegerv2)(GLenum, GLint*);
    if (!p_glMatrixMode) {
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        p_glMatrixMode = (decltype(p_glMatrixMode))GetProcAddress(gl, "glMatrixMode");
        p_glPushMatrix = (decltype(p_glPushMatrix))GetProcAddress(gl, "glPushMatrix");
        p_glPopMatrix = (decltype(p_glPopMatrix))GetProcAddress(gl, "glPopMatrix");
        p_glLoadIdentity = (decltype(p_glLoadIdentity))GetProcAddress(gl, "glLoadIdentity");
        p_glViewport = (decltype(p_glViewport))GetProcAddress(gl, "glViewport");
        p_glGetIntegerv2 = (decltype(p_glGetIntegerv2))GetProcAddress(gl, "glGetIntegerv");
    }
    GLint mode = GL_MODELVIEW, vp[4];
    p_glGetIntegerv2(GL_MATRIX_MODE, &mode);
    p_glGetIntegerv2(GL_VIEWPORT, vp);
    p_glViewport(0, 0, g_texW, g_texH);
    p_glMatrixMode(GL_PROJECTION);
    p_glPushMatrix();
    p_glLoadIdentity();
    g_gl.Frustum(g_zn * tan(g_curFov.angleLeft), g_zn * tan(g_curFov.angleRight), g_zn * tan(g_curFov.angleDown),
                 g_zn * tan(g_curFov.angleUp), g_zn, g_zf);
    // inv(E) = [Q^T | -Q^T t], column-major
    float invE[16] = {};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) invE[c * 4 + r] = g_Q[c * 3 + r];
    for (int r = 0; r < 3; r++)
        invE[12 + r] = -(g_Q[0 * 3 + r] * g_t[0] + g_Q[1 * 3 + r] * g_t[1] + g_Q[2 * 3 + r] * g_t[2]);
    invE[15] = 1;
    Weapons_Draw(invE);
    float worldView[16];
    if (g_worldViewValid) {
        memcpy(worldView, g_worldView, sizeof worldView);
        Weapons_DrawWarnings(worldView);
    }
    p_glMatrixMode(GL_PROJECTION);
    p_glPopMatrix();
    p_glMatrixMode(mode);
    p_glViewport(vp[0], vp[1], vp[2], vp[3]);
}

bool VR_IsActive() { return g_vrActive; }

// Pick this frame's eye (alternate-eye rendering) and its pose; idempotent within a frame.
bool VR_ChooseEye(KvrPose* out) {
    if (!g_vrActive) return false;
    if (!g_eyeChosen) {
        KvrHostState hs;
        if (!ReadHost(hs) || !hs.running) return false;
        g_curEye = g_worldFrames & 1;
        g_curPose = hs.eye[g_curEye];
        g_curFov = hs.fov[g_curEye];
        QuatToRows(g_curPose.quat, g_Q);
        memcpy(g_t, g_curPose.pos, sizeof g_t);
        g_eyeChosen = true;
    }
    *out = g_curPose;
    return true;
}

// ---------------------------------------------------------------- hooks
void VR_Init(const VrGl& gl, void (*log)(const char*, ...)) {
    g_gl = gl;
    g_log = log;
}

bool VR_OnPerspective(GLdouble fovy, GLdouble aspect, GLdouble zn, GLdouble zf) {
    g_camState = 0;
    if (!g_vrActive) return false;
    // world/dialogue cameras only: GUI 3D (portraits, menu model) uses fovy 22.7 or non-4:3 aspect
    double expect = (double)g_texW / g_texH;
    if (fovy < 25.0 || fabs(aspect - expect) > 0.05) return false;
    g_gameFovy = fovy;
    g_gameAspect = aspect;

    KvrPose unused;
    if (!VR_ChooseEye(&unused)) return false;
    g_zn = zn;
    g_zf = zf;
    g_inWorldPass = true;

    g_gl.Frustum(zn * tan(g_curFov.angleLeft), zn * tan(g_curFov.angleRight),
                 zn * tan(g_curFov.angleDown), zn * tan(g_curFov.angleUp), zn, zf);
    g_worldThisFrame = true;
    g_camState = 1;
    return true;
}

// The camera's glMultMatrixf(R) is held back and issued together with its glTranslatef(-p), so the pair can be
// swapped for the last gameplay camera during dialogue (see below).
bool VR_OnMultMatrixf(GLenum mode, const GLfloat* m) {
    if (g_camState != 1 || mode != GL_MODELVIEW) return false;
    if (EngineCam_Applied()) {  // the engine is already rendering from the eye pose (and culling with it)
        memcpy(g_engR, m, sizeof g_engR);
        g_camState = 4;  // remember its translate too (warning glyphs draw in world space)
        return false;
    }
    memcpy(g_R, m, sizeof g_R);
    g_camState = 2;
    return true;
}

// Dialogue / cutscene freeze: gameplay cameras use camerastyle's viewangle (120 via the Override); anything else
// is a scripted camera. In first person we keep the view at the last gameplay (head) camera while the scripted
// camera stays nearby, so conversations happen around you instead of cutting between angles.
static bool g_havePlayCam;
static float g_camPos[3];   // camera position used for this frame's view (head hook in first person)
static bool g_camPosValid;
static float g_playR[16], g_playP[3];
static const double kGameplayFov = 170.0;  // camerastyle.2da viewangle in Override (culling cone only)
static const float kFreezeRadius = 12.0f;  // meters; scripted cameras further away are a cut elsewhere

bool VR_OnTranslatef(GLenum mode, GLfloat x, GLfloat y, GLfloat z) {
    if (g_camState == 4 && mode == GL_MODELVIEW) {  // engine camera: view = R * T(x, y, z), passed through untouched
        memcpy(g_worldView, g_engR, sizeof g_worldView);
        for (int r = 0; r < 3; r++) g_worldView[12 + r] += g_engR[0 * 4 + r] * x + g_engR[1 * 4 + r] * y + g_engR[2 * 4 + r] * z;
        g_worldViewValid = true;
        g_camState = 0;
        return false;
    }
    if (g_camState != 2 || mode != GL_MODELVIEW) return false;
    float p[3] = {-x, -y, -z};
    bool gameplay = fabs(g_gameFovy - kGameplayFov) < 0.5;
    if (gameplay) {
        EngineCam_OnView(g_R, p);
        memcpy(g_playR, g_R, sizeof g_playR);
        memcpy(g_playP, p, sizeof g_playP);
        g_havePlayCam = true;
    } else if (g_havePlayCam) {
        float dx = p[0] - g_playP[0], dy = p[1] - g_playP[1], dz = p[2] - g_playP[2];
        if (dx * dx + dy * dy + dz * dz < kFreezeRadius * kFreezeRadius) {
            memcpy(g_R, g_playR, sizeof g_R);
            memcpy(p, g_playP, sizeof p);
        }
    }
    if (g_shm) g_shm->cinematic = gameplay ? 0 : 1;

    // view = inv(E) * R * T(-p); inv(E) = [Q^T | -Q^T t]. Column-major: M[c*4 + r].
    float invE[16] = {};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) invE[c * 4 + r] = g_Q[c * 3 + r];
    for (int r = 0; r < 3; r++)
        invE[12 + r] = -(g_Q[0 * 3 + r] * g_t[0] + g_Q[1 * 3 + r] * g_t[1] + g_Q[2 * 3 + r] * g_t[2]);
    invE[15] = 1;
    float out[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += invE[k * 4 + r] * g_R[c * 4 + k];
            out[c * 4 + r] = v;
        }
    g_gl.MultMatrixf(out);
    g_gl.Translatef(-p[0], -p[1], -p[2]);

    // camera world transform C = [R^T | p]; vertex programs get C' = C * E
    for (int r = 0; r < 3; r++) {
        float Rt[3] = {g_R[r * 4 + 0], g_R[r * 4 + 1], g_R[r * 4 + 2]};  // row r of R^T = column r of R
        for (int c = 0; c < 3; c++) g_envRows[r][c] = Rt[0] * g_Q[0 * 3 + c] + Rt[1] * g_Q[1 * 3 + c] + Rt[2] * g_Q[2 * 3 + c];
        g_envRows[r][3] = p[r] + Rt[0] * g_t[0] + Rt[1] * g_t[1] + Rt[2] * g_t[2];
    }
    memcpy(g_camPos, p, sizeof g_camPos);
    g_camPosValid = true;
    g_camState = 3;
    return true;
}

static void NoteBoneParam(GLuint index, const GLfloat* v);
bool VR_OnEnvParam(GLenum target, GLuint index, GLfloat* v) {
    if (target == GL_VERTEX_PROGRAM_ARB && index >= 18 && index <= 20) NoteBoneParam(index, v);
    if (g_camState != 3 || target != GL_VERTEX_PROGRAM_ARB || index < 89 || index > 92) return false;
    if (index == 89) { g_camState = 0; return false; }
    memcpy(v, g_envRows[92 - index], 4 * sizeof(float));
    return true;
}

void VR_OnOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t) {
    g_inWorldPass = false;  // post-processing / GUI from here on: nothing here is the player's body
    // first unit-square ortho after the world = GUI start; scene, shadows and glow are composited by now
    if (!g_worldThisFrame || g_eyeCaptured || l != 0.0 || r != 1.0 || b != 0.0 || t != 1.0) return;
    if (g_probeState == 1) {  // line-of-sight probe: depth at the view center vs the distance to the target point
        static void(APIENTRY* p_rp)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
        if (!p_rp) p_rp = (decltype(p_rp))GetProcAddress(GetModuleHandleA("opengl32.dll"), "glReadPixels");
        double tl = tan(g_curFov.angleLeft), tr = tan(g_curFov.angleRight), td = tan(g_curFov.angleDown), tu = tan(g_curFov.angleUp);
        GLint x = (GLint)((0 - tl) / (tr - tl) * g_texW), y = (GLint)((0 - td) / (tu - td) * g_texH);
        float d = 1.0f;
        p_rp(x, y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &d);
        double zn = g_zn, zf = g_zf, ndc = 2.0 * d - 1.0;
        double eyeZ = 2.0 * zn * zf / (zf + zn - ndc * (zf - zn));
        g_probeDepth = d >= 0.99999f ? 1e6f : (float)eyeZ;
        // the target is a surface point (what the shot looks at): reaching its near side counts
        double margin = g_probeDist * 0.15 > 1.0 ? g_probeDist * 0.15 : 1.0;
        g_probeClear = d >= 0.99999f || eyeZ >= g_probeDist - margin;
        g_probeState = 2;
    } else if (NowMs() >= g_pickUntilMs) {  // a pick frame looks along the hand: never shown in the headset
        DrawHandItems();
        Capture(g_curEye, &g_curPose, &g_curFov);
        if (g_curEye == 0) Mirror_KeepEye(g_texW, g_texH, g_gl.GetProcAddress);  // desktop mirror (left eye)
    }
    g_eyeCaptured = true;
    ClearForGui();
}

void VR_OnSwap(HDC dc) {
    Weapons_Update();
    if (Features().tracePosition && g_worldThisFrame) {
        static DWORD last;
        if (GetTickCount() - last > 2000) {
            last = GetTickCount();
            g_log("modelcull: nearest object origin to the player %.3f m (player %.2f %.2f %.2f)", sqrtf(g_nearestOwn),
                  g_playerPos[0], g_playerPos[1], g_playerPos[2]);
        }
    }
    g_nearestOwn = 1e9f;
    NearEyeFrameEnd();
    g_hookCount = Anim_PlayerHooks(g_hooks, 8);
    g_ownNodeCur ^= 1;  // this frame's bones become "last frame's"
    g_ownNodeCount[g_ownNodeCur] = 0;
    RefreshPlayerPos();
    Combat_EndFrame();
    static double lastStat, lastSwap;
    static unsigned frames, worldFrames;
    double now = NowMs();
    if (lastSwap > 0 && now - lastSwap > 250) g_log("STALL frame gap %.0f ms (vr %d)", now - lastSwap, g_vrActive);
    lastSwap = now;
    frames++;
    if (g_worldThisFrame) worldFrames++;
    if (now - lastStat > 1000) {
        if (g_vrActive) g_log("stats: %u fps, %u world, eyes %u/%u, panel %u", frames, worldFrames,
                              g_shm ? g_shm->eyes[0].frameId : 0, g_shm ? g_shm->eyes[1].frameId : 0, g_shm ? g_shm->panel.frameId : 0);
        frames = worldFrames = 0;
        lastStat = now;
    }
    if (EnsureShm()) {
        g_shm->gameAlive++;
        if (!g_exportReady && !g_exportFailed && g_shm->host.running) {
            g_exportReady = InitDevice();
            g_exportFailed = !g_exportReady;
        }
        if (g_exportReady) {
            RECT rc;
            HWND gw = WindowFromDC(dc);
            if (GetClientRect(gw, &rc)) {
                int w = rc.right - rc.left, h = rc.bottom - rc.top;
                // a minimized window (fullscreen KotOR minimizes when it loses focus) has nothing to copy: the
                // headset gets no frames at all - say so instead of failing silently (issue #1, 2026-11-09)
                static DWORD lastTold;
                if ((w <= 0 || h <= 0 || IsIconic(gw)) && GetTickCount() - lastTold > 5000) {
                    lastTold = GetTickCount();
                    g_log("export: the game window is minimized (%dx%d) - nothing to send to the headset. Run KotOR "
                          "windowed: swkotor.ini [Graphics Options] FullScreen=0", w, h);
                }
                if (w > 0 && h > 0 && (w != g_texW || h != g_texH)) {
                    if (!CreateSlots(w, h)) { g_log("export: slot creation failed"); DestroySlots(); g_exportReady = false; g_exportFailed = true; }
                }
            }
        }
        if (g_exportReady) {
            if (g_worldThisFrame && !g_eyeCaptured && NowMs() >= g_pickUntilMs && g_probeState != 1) Capture(g_curEye, &g_curPose, &g_curFov);
            Capture(SLOT_PANEL, nullptr, nullptr);
            if (g_guiOnlyThisFrame) Mirror_Draw();  // the window shows the eye under the GUI (the panel is taken)
            // a probe that never got its depth read (no world / GUI pass this frame): give up after 3 frames as
            // "clear" so a cutscene can never hold the headset on a stale frame
            static int probeFrames;
            if (g_probeState == 1) {
                if (++probeFrames > 3) {
                    g_probeClear = true;
                    g_probeDepth = 1e6f;
                    g_probeState = 2;
                    g_log("cutscene: line-of-sight probe not answered, staying first person");
                }
            } else {
                probeFrames = 0;
            }
            if (g_worldThisFrame) {
                g_shm->lastWorldFrame = g_shm->gameAlive;
                g_worldFrames++;
            }
        }
        static bool f8, f9;
        bool k8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0, k9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (k8 && !f8) g_shm->recenterReq = 1;
        if (k9 && !f9) g_shm->showPanel = !g_shm->showPanel;
        f8 = k8;
        f9 = k9;
        g_vrActive = g_exportReady && g_shm->host.running;
    }
    g_worldThisFrame = false;
    g_worldViewValid = false;
    g_eyeChosen = false;
    g_camPosValid = false;
    g_eyeCaptured = false;
    g_guiOnlyThisFrame = false;
    g_camState = 0;
}

int VR_OwnNodesThisFrame(float (*out)[3], int max) {
    int n = g_ownNodeCount[g_ownNodeCur] < max ? g_ownNodeCount[g_ownNodeCur] : max;
    memcpy(out, g_ownNodes[g_ownNodeCur], n * 12);
    return n;
}

void VR_OwnNode(const float world[3]) {
    int& n = g_ownNodeCount[g_ownNodeCur];
    if (n < 128) memcpy(g_ownNodes[g_ownNodeCur][n++], world, 12);
}

bool VR_IsOurTexture(GLuint) { return false; }  // export no longer owns GL textures (PBO readback)

bool VR_InCapture() { return g_inCapture; }

const KvrShared* VR_Shared() { return g_shm; }

void VR_ClientSize(int* w, int* h) {
    *w = g_texW;
    *h = g_texH;
}

bool VR_WorldPointerToClient(const float aimPos[3], const float aimQuat[4], float* x, float* y) {
    if (g_texW <= 0) return false;
    float Q[9];
    QuatToRows(aimQuat, Q);
    // forward = Q * (0, 0, -1), then into the game camera's frame: our view heading differs from it by the offset
    float d0[3] = {-Q[2], -Q[5], -Q[8]};
    float off = EngineCam_ViewYawOffset(), co = cosf(off), so = sinf(off);
    // right-handed rotation about +Y by +off (turns -Z toward -X, i.e. left)
    float d[3] = {co * d0[0] + so * d0[2], d0[1], -so * d0[0] + co * d0[2]};
    float p0[3] = {co * aimPos[0] + so * aimPos[2], aimPos[1], -so * aimPos[0] + co * aimPos[2]};
    aimPos = p0;
    float a[3] = {aimPos[0] + d[0] * 10.0f, aimPos[1] + d[1] * 10.0f, aimPos[2] + d[2] * 10.0f};
    if (a[2] > -0.05f) return false;  // pointing behind the game camera
    double t = tan(g_gameFovy * 3.14159265358979 / 360.0);
    double nx = (a[0] / -a[2]) / (t * g_gameAspect);
    double ny = (a[1] / -a[2]) / t;
    if (nx < -1 || nx > 1 || ny < -1 || ny > 1) return false;  // outside the game's view: leave the cursor alone
    *x = (float)((nx * 0.5 + 0.5) * g_texW);
    *y = (float)((0.5 - ny * 0.5) * g_texH);
    return true;
}

// A point in tracking space (e.g. where the hand's ray enters the object it points at, pick.cpp) to the game window,
// through the same camera frame as VR_WorldPointerToClient - a real point, so no parallax between hand and eyes.
bool VR_TrackingPointToClient(const float p[3], float* x, float* y) {
    if (g_texW <= 0) return false;
    float off = EngineCam_ViewYawOffset(), co = cosf(off), so = sinf(off);
    float a[3] = {co * p[0] + so * p[2], p[1], -so * p[0] + co * p[2]};
    if (a[2] > -0.05f) return false;
    double t = tan(g_gameFovy * 3.14159265358979 / 360.0);
    double nx = (a[0] / -a[2]) / (t * g_gameAspect);
    double ny = (a[1] / -a[2]) / t;
    if (nx < -1 || nx > 1 || ny < -1 || ny > 1) return false;
    *x = (float)((nx * 0.5 + 0.5) * g_texW);
    *y = (float)((0.5 - ny * 0.5) * g_texH);
    return true;
}

static bool g_showOwnBody;  // dead: the body is the subject of the death camera
void VR_SetShowOwnBody(bool show) { g_showOwnBody = show; }
bool VR_ShowingOwnBody() { return g_showOwnBody; }

int VR_IsOwnBody(const float t[3], int depth) {
    if (g_showOwnBody) return 0;
    if (Features().modelCull) {
        // exact: the model hierarchy whose world transform sits on the controlled creature (its body, head, hair and
        // held items are all children of it); nothing else in the scene is touched
        if (!g_vrActive || !g_inWorldPass || !g_playerPosValid) return 0;
        float dx = t[0] - g_playerPos[0], dy = t[1] - g_playerPos[1], dz = t[2] - g_playerPos[2];
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < g_nearestOwn) g_nearestOwn = d2;
        float h2 = dx * dx + dy * dy;
        // the body hierarchy's root (feet): hide it whole. Matched against where the body model is actually drawn
        // (within 5 cm): the old 25 cm radius around the creature also caught any model whose origin you stood on -
        // a room's origin is a point on its floor, and walking over it hid the whole room (floor gone, space below;
        // 2026-11-05)
        float bp[3];
        static int oldRadius = -1;  // [Debug] OldOwnBodyRadius=1: the old rule (before / after comparisons)
        if (oldRadius < 0) oldRadius = GetPrivateProfileIntA("Debug", "OldOwnBodyRadius", 0, Features_IniPath());
        bool exact = !oldRadius && Anim_PlayerBodyPos(bp);
        float bx = t[0] - bp[0], by = t[1] - bp[1], bz = t[2] - bp[2];
        if (exact ? (bx * bx + by * by + bz * bz < 0.05f * 0.05f) : (h2 < 0.25f * 0.25f && fabsf(dz) < 0.35f)) {
            g_rootDepth = depth;
            return 2;
        }
        if (depth == g_rootDepth) memcpy(g_lastRootT, t, sizeof g_lastRootT);
        // a model rooted on one of the body's attachment points (head on the headhook, weapons in the hands), as
        // the animation has them this frame: the bone list below misses them in attacks, where the skeleton is
        // skinned on the GPU (2026-11-05: your own head drawn in your eyes mid-swing)
        if (depth == g_rootDepth) {
            // ... and anything rooted inside your own head (your head model, wherever the animation puts it):
            // in first person nothing else has its origin within 35 cm of your eye
            float eye[3];
            if (!g_showOwnBody && EngineCam_EyeWorld(eye)) {
                float ex = t[0] - eye[0], ey = t[1] - eye[1], ez = t[2] - eye[2];
                if (ex * ex + ey * ey + ez * ez < 0.35f * 0.35f) return 2;
            }
            for (int i = 0; i < g_hookCount; i++) {
                float ex = t[0] - g_hooks[i][0], ey = t[1] - g_hooks[i][1], ez = t[2] - g_hooks[i][2];
                if (ex * ex + ey * ey + ez * ez < 0.10f * 0.10f) return 2;
            }
        }
        int prev = g_ownNodeCur ^ 1;
        if (g_ownNodeCount[prev] > 0) {
            // Exact: a model rooted on one of the body's bones (held weapons on the hands, the head on the neck) -
            // wherever an attack swings it. Scenery that merely stands close (crates, consoles, door frames) stays.
            if (depth == g_rootDepth && h2 < 3.0f * 3.0f) {
                for (int i = 0; i < g_ownNodeCount[prev]; i++) {
                    const float* n = g_ownNodes[prev][i];
                    // only the hands / arms / head carry models (weapons, the head): leg and foot bones sit at floor
                    // level, where props and decals next to you have their roots (they vanished as you walked past)
                    if (n[2] < g_playerPos[2] + 0.6f) continue;
                    float ex = t[0] - n[0], ey = t[1] - n[1], ez = t[2] - n[2];
                    if (ex * ex + ey * ey + ez * ez < 0.08f * 0.08f) return 2;
                }
            }
            return 0;
        }
        // no bones seen yet (first frames): the old radius rules
        // running / combat moves can put the drawn root a bit away from the server position for a frame: at the
        // model-root depth (whole objects only) accept up to 0.6 m, which only a creature standing inside you reaches
        if (depth == g_rootDepth && h2 < 0.6f * 0.6f && fabsf(dz) < 0.6f) return 2;
        if (depth == g_rootDepth && h2 < 1.2f * 1.2f && fabsf(dz) < 1.0f) {  // near miss: log (popping diagnosis)
            static DWORD last;
            if (GetTickCount() - last > 1000) {
                last = GetTickCount();
                g_log("modelcull: near miss root at %.2f m horizontal, %.2f m vertical", sqrtf(h2), dz);
            }
        }
        // the head is a separate model attached at the neck: its origin sits right above the feet at head height
        if (h2 < 0.3f * 0.3f && dz > 0.9f && dz < 2.3f) return 2;
        // held weapons are separate models at the hand: other model roots (same stack depth as the body's root) that
        // sit within arm's reach. Only whole roots: parts of a nearby NPC are deeper in its own hierarchy.
        return depth == g_rootDepth && h2 < 0.75f * 0.75f && dz > 0.1f && dz < 1.9f ? 2 : 0;
    }
    if (!g_camPosValid || !g_vrActive) return 0;
    float dx = t[0] - g_camPos[0], dy = t[1] - g_camPos[1], dz = t[2] - g_camPos[2];
    // KotOR world is Z-up; the camera sits at the character's camera hook, the body is a column below it
    // 0.7 m: covers the head/hair and held items swinging forward in the walk cycle
    return dx * dx + dy * dy < 0.7f * 0.7f && dz > -2.2f && dz < 0.8f ? 2 : 0;
}

void VR_EngineCamApplied(const float basePos[3]) {
    memcpy(g_camPos, basePos, sizeof g_camPos);
    g_camPosValid = true;
}

void VR_SetCinematic(bool on) {
    if (g_shm) g_shm->cinematic = on ? 1 : 0;
}

void VR_RequestLosProbe(float distance) {
    g_probeDist = distance;
    g_probeState = 1;
}

float VR_LosProbeDepth() { return g_probeDepth; }
int VR_LosProbeResult() {
    if (g_probeState != 2) return -1;
    g_probeState = 0;
    return g_probeClear ? 1 : 0;
}

void VR_BeginAimPick(const KvrPose& aim, double seconds) {
    g_pickPose = aim;
    g_pickUntilMs = NowMs() + seconds * 1000.0;
}

bool VR_AimPick(KvrPose* out) {
    if (NowMs() >= g_pickUntilMs) return false;
    *out = g_pickPose;
    return true;
}

void VR_ShowText(int slot, unsigned anchor, const float pos[3], const float quat[4], float width, const char* text,
                 int highlight) {
    if (!g_shm || slot < 0 || slot >= KVR_TEXT_PANELS) return;
    KvrTextPanel& t = g_shm->text[slot];
    t.anchor = anchor;
    memcpy(t.pose.pos, pos, sizeof t.pose.pos);
    memcpy(t.pose.quat, quat, sizeof t.pose.quat);
    t.width = width;
    t.highlight = highlight;
    t.pickable = 0;
    strncpy_s(t.text, text, _TRUNCATE);
    MemoryBarrier();
    t.serial = t.serial + 1;
}

void VR_SetTextPickable(int slot, bool on) {
    if (!g_shm || slot < 0 || slot >= KVR_TEXT_PANELS) return;
    g_shm->text[slot].pickable = on ? 1 : 0;
}

void VR_MovieFrame(const unsigned char* bgrx, int w, int h) {
    Slot& s = g_slots[SLOT_PANEL];
    if (!g_exportReady || !s.shared || !g_shm) return;
    // fit into the panel texture (movies are smaller than the window; scale down by whole steps if not)
    int step = 1;
    while (w / step > g_texW || h / step > g_texH) step++;
    int ow = w / step, oh = h / step;
    static std::vector<unsigned char> rgba;
    rgba.resize((size_t)ow * oh * 4);
    for (int y = 0; y < oh; y++) {
        const unsigned char* src = bgrx + (size_t)(y * step) * w * 4;
        unsigned char* dst = &rgba[(size_t)(oh - 1 - y) * ow * 4];  // bottom-up, like the GL frames
        for (int x = 0; x < ow; x++, dst += 4) {
            const unsigned char* p = src + (size_t)x * step * 4;
            dst[0] = p[2];
            dst[1] = p[1];
            dst[2] = p[0];
            dst[3] = 255;
        }
    }
    if (s.mutex->AcquireSync(KEY_GAME, 0) != S_OK) return;  // host still holds the last one; drop this frame
    D3D11_BOX box = {0, 0, 0, (UINT)ow, (UINT)oh, 1};
    g_ctx->UpdateSubresource(s.shared, 0, &box, rgba.data(), ow * 4, 0);
    g_shm->panel.width = ow;
    g_shm->panel.height = oh;
    g_shm->panel.flags = KVR_SLOT_MOVIE;
    g_shm->panel.frameId++;
    s.mutex->ReleaseSync(KEY_HOST);
    g_ctx->Flush();
    // the readbacks queued before the movie belong to the old scene
    s.pending[0] = s.pending[1] = false;
}

void VR_Flash(float r, float g, float b, float strength, float seconds) {
    if (!g_shm) return;
    g_shm->flash.rgb[0] = r;
    g_shm->flash.rgb[1] = g;
    g_shm->flash.rgb[2] = b;
    g_shm->flash.strength = strength;
    g_shm->flash.duration = seconds;
    MemoryBarrier();
    g_shm->flash.serial = g_shm->flash.serial + 1;
}

void VR_MovieEnded() {
    if (g_shm) g_shm->panel.flags &= ~KVR_SLOT_MOVIE;
}

void VR_HideText(int slot) {
    if (!g_shm || slot < 0 || slot >= KVR_TEXT_PANELS) return;
    g_shm->text[slot].anchor = KVR_ANCHOR_HIDDEN;
    MemoryBarrier();
    g_shm->text[slot].serial = g_shm->text[slot].serial + 1;
}

void VR_Haptic(int hand, float amplitude, float duration, float frequency) {
    if (!g_shm || hand < 0 || hand > 1) return;
    KvrHaptic& h = g_shm->haptic[hand];
    h.amplitude = amplitude;
    h.duration = duration;
    h.frequency = frequency;
    MemoryBarrier();
    h.serial = h.serial + 1;
}

float VR_HeadYaw() {
    if (!g_vrActive || !g_shm) return 0.0f;
    KvrHostState hs;
    if (!ReadHost(hs)) return 0.0f;
    // average the two eyes' orientation; forward = q * (0,0,-1) in GL camera space (x right, -z forward)
    const float* q = hs.eye[0].quat;
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float fwdRight = -2 * (x * z + w * y);
    float fwdForward = 1 - 2 * (x * x + y * y);
    return atan2f(-fwdRight, fwdForward);
}

void VR_HeadPos(float out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (!g_shm) return;
    KvrHostState hs;
    if (!ReadHost(hs)) return;
    for (int i = 0; i < 3; i++) out[i] = (hs.eye[0].pos[i] + hs.eye[1].pos[i]) * 0.5f;
}

static float g_bone0[3];  // first bone origin (model space) written since the last draw
static int g_boneFresh;

// Own-body culling, per draw: where does this draw's model origin sit relative to the first-person camera?
// The modelview at draw time is view(eye) * model, so its translation is the model origin in eye space; the eye
// pose E (in the camera's frame) takes it back to the camera frame, where the body is a column below the camera.
// Works for nested node hierarchies and skinned meshes alike (their origin is the model root at the feet).
// Vertex array state (client-side arrays: the ini disables VBOs), recorded by the glVertexPointer hook.
static const BYTE* g_vtxPtr;
static GLint g_vtxSize;
static GLsizei g_vtxStride;
void VR_OnVertexPointer(GLint size, GLenum type, GLsizei stride, const void* ptr) {
    g_vtxPtr = type == GL_FLOAT && size >= 3 ? (const BYTE*)ptr : nullptr;
    g_vtxSize = size;
    g_vtxStride = stride ? stride : size * (GLsizei)sizeof(float);
}

static bool InBodyColumn(const float oc[3]) {
    return oc[0] * oc[0] + oc[2] * oc[2] < 1.0f * 1.0f && oc[1] > -2.2f && oc[1] < 0.8f;
}

// eye-space point -> camera (recenter origin) frame
static void EyeToCam(const float pe[3], float oc[3]) {
    for (int r = 0; r < 3; r++) oc[r] = g_Q[r * 3 + 0] * pe[0] + g_Q[r * 3 + 1] * pe[1] + g_Q[r * 3 + 2] * pe[2] + g_t[r];
}

// Test harness (simcmd "neareye on"): world geometry drawn within 12 cm of the eye - your own head clipping into
// view (2026-11-05 playtest). Samples a few vertices of every draw that is not hidden; logs once a second.
static bool g_nearEyeOn;
static int g_nearEyeDraws, g_nearEyeFrames;
void VR_SetNearEyeWatch(bool on) {
    g_nearEyeOn = on;
    g_nearEyeDraws = g_nearEyeFrames = 0;
}

// Your own head, whatever hierarchy it is drawn under: a draw whose origin is within 35 cm of the eye and which has
// geometry within 12 cm of it is skipped (2026-11-05: in attacks the head's parts were drawn in your eyes - rooted
// away from where the body's hooks and bones say, so the model-level rules missed them). Room meshes have their
// origins far away, so a wall you lean into stays.
bool VR_InsideHead(GLsizei count, GLenum type, const void* indices) {
    if (g_showOwnBody || !g_vrActive || !g_inWorldPass || !g_vtxPtr || count <= 0) return false;
    static void(APIENTRY* p_glGetFloatv)(GLenum, GLfloat*);
    if (!p_glGetFloatv) p_glGetFloatv = (decltype(p_glGetFloatv))GetProcAddress(GetModuleHandleA("opengl32.dll"), "glGetFloatv");
    float mv[16];
    p_glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    if (mv[12] * mv[12] + mv[13] * mv[13] + mv[14] * mv[14] > 0.35f * 0.35f) {
        // ... or rooted in the column under your head (chest to feet): a body mesh the hierarchy match missed (switching characters, then the
        // new one using a door: its body drawn with your eye inside its head; 2026-11-05 tutorial run)
        float r[3];  // the draw's origin relative to the eye, tracking frame (y up)
        for (int k = 0; k < 3; k++) r[k] = g_Q[k * 3 + 0] * mv[12] + g_Q[k * 3 + 1] * mv[13] + g_Q[k * 3 + 2] * mv[14];
        if (r[0] * r[0] + r[2] * r[2] > 0.25f * 0.25f || r[1] > -0.3f || r[1] < -2.2f) return false;  // chest to feet
    }
    __try {
        for (int s = 0; s < 8; s++) {
            GLsizei k = (GLsizei)((long long)count * s / 8);
            unsigned idx = type == GL_UNSIGNED_SHORT ? ((const unsigned short*)indices)[k]
                         : type == GL_UNSIGNED_INT ? ((const unsigned*)indices)[k] : ((const unsigned char*)indices)[k];
            const float* v = (const float*)(g_vtxPtr + (size_t)idx * g_vtxStride);
            float pe[3];
            for (int r = 0; r < 3; r++) pe[r] = mv[r] * v[0] + mv[4 + r] * v[1] + mv[8 + r] * v[2] + mv[12 + r];
            if (pe[0] * pe[0] + pe[1] * pe[1] + pe[2] * pe[2] < 0.12f * 0.12f) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

bool VR_NoteDrawn(GLsizei count, GLenum type, const void* indices) {
    if (!g_nearEyeOn || !g_vrActive || !g_inWorldPass || !g_vtxPtr || count <= 0) return false;
    static void(APIENTRY* p_glGetFloatv)(GLenum, GLfloat*);
    if (!p_glGetFloatv) p_glGetFloatv = (decltype(p_glGetFloatv))GetProcAddress(GetModuleHandleA("opengl32.dll"), "glGetFloatv");
    float mv[16];
    p_glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    __try {
        for (int s = 0; s < 8; s++) {
            GLsizei k = (GLsizei)((long long)count * s / 8);
            unsigned idx = type == GL_UNSIGNED_SHORT ? ((const unsigned short*)indices)[k]
                         : type == GL_UNSIGNED_INT ? ((const unsigned*)indices)[k] : ((const unsigned char*)indices)[k];
            const float* v = (const float*)(g_vtxPtr + (size_t)idx * g_vtxStride);
            float pe[3];
            for (int r = 0; r < 3; r++) pe[r] = mv[r] * v[0] + mv[4 + r] * v[1] + mv[8 + r] * v[2] + mv[12 + r];
            if (pe[0] * pe[0] + pe[1] * pe[1] + pe[2] * pe[2] < 0.12f * 0.12f) {
                // an enemy's weapon swung through your view is theirs, not your avatar: not counted
                {
                    float e[3], hp[3];
                    if (Melee_NearestEnemy(g_playerPos, e))
                        if (void* em = Anim_ModelAt(e))
                            for (const char* hn : {"rhand", "lhand"})
                                if (Anim_PartPos(em, hn, hp)) {
                                    float ex = g_lastRootT[0] - hp[0], ey = g_lastRootT[1] - hp[1], ez = g_lastRootT[2] - hp[2];
                                    if (ex * ex + ey * ey + ez * ez < 0.1f * 0.1f) {
                                        static int told;
                                        if (told++ < 3) g_log("neareye: an enemy's weapon passed through your view (not counted)");
                                        return false;
                                    }
                                }
                }
                // whose model: its root against your feet and your body's bones (last frame)
                float dx = g_lastRootT[0] - g_playerPos[0], dy = g_lastRootT[1] - g_playerPos[1], dz = g_lastRootT[2] - g_playerPos[2];
                float best = 99;
                int prev = g_ownNodeCur ^ 1;
                for (int i = 0; i < g_ownNodeCount[prev]; i++) {
                    const float* n = g_ownNodes[prev][i];
                    float ex = g_lastRootT[0] - n[0], ey = g_lastRootT[1] - n[1], ez = g_lastRootT[2] - n[2];
                    float d = sqrtf(ex * ex + ey * ey + ez * ez);
                    if (d < best) best = d;
                }
                float hk = 99;
                for (int h = 0; h < g_hookCount; h++) {
                    float ex = g_lastRootT[0] - g_hooks[h][0], ey = g_lastRootT[1] - g_hooks[h][1], ez = g_lastRootT[2] - g_hooks[h][2];
                    float d = sqrtf(ex * ex + ey * ey + ez * ez);
                    if (d < hk) hk = d;
                }
                // only your own body counts: its models are rooted in your head (within 35 cm of the eye) or in the
                // column under it. A pillar or console you walked your face into, or an enemy pressed against you mid-
                // fight, is rooted off to the side: logged, not counted (2026-11-05 tutorial runs)
                float org[3];
                VR_DrawOrigin(org);
                for (int k = 0; k < 3; k++) org[k] -= g_t[k];  // relative to the eye
                bool head = org[0] * org[0] + org[1] * org[1] + org[2] * org[2] < 0.35f * 0.35f;
                bool column = org[0] * org[0] + org[2] * org[2] < 0.3f * 0.3f && org[1] < 0;
                if (!head && !column) {
                    static int told;
                    if (told++ < 5)
                        g_log("neareye: something else at your face, rooted %.2f m to the side (not counted)",
                              sqrtf(org[0] * org[0] + org[2] * org[2]));
                    return false;
                }
                g_nearEyeDraws++;
                static int logged;

                if (logged++ < 12) {
                    float oc[3];
                    VR_DrawOrigin(oc);  // the draw's own origin, in the tracking frame (y up, -z forward)
                    g_log("neareye: draw count %d, vertex at eye (%.2f %.2f %.2f); draw origin (tracking) %.2f %.2f %.2f; "
                          "last root %.2f m from your feet (up %.2f), %.2f m from your nearest bone, %.2f m from the nearest of %d "
                          "hooks; hook0 (%.2f %.2f %.2f) root (%.2f %.2f %.2f)",
                          (int)count, pe[0], pe[1], pe[2], oc[0], oc[1], oc[2], sqrtf(dx * dx + dy * dy), dz, best, hk,
                          g_hookCount, g_hooks[0][0], g_hooks[0][1], g_hooks[0][2], g_lastRootT[0], g_lastRootT[1], g_lastRootT[2]);
                }
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static void NearEyeFrameEnd() {
    if (!g_nearEyeOn) return;
    static DWORD last;
    static int framesWith;
    static int drawsAtFrameStart;
    if (g_nearEyeDraws > drawsAtFrameStart) framesWith++;
    drawsAtFrameStart = g_nearEyeDraws;
    g_nearEyeFrames++;
    if (GetTickCount() - last > 1000) {
        last = GetTickCount();
        if (framesWith) g_log("neareye: %d of %d frames drew geometry within 12 cm of the eye", framesWith, g_nearEyeFrames);
        framesWith = 0;
        g_nearEyeFrames = 0;
        g_nearEyeDraws = drawsAtFrameStart = 0;
    }
}

// CPU-skinned meshes have world-space vertices under a view-only modelview, so their origin says nothing:
// test actual vertices of the draw instead.
bool VR_ShouldSkipElements(GLsizei count, GLenum type, const void* indices) {
    if (Features().modelCull) return false;  // whole hierarchy already skipped exactly
    if (!g_vrActive || !g_inWorldPass || !g_camPosValid || !g_vtxPtr || count <= 0) return false;
    static void(APIENTRY* p_glGetFloatv)(GLenum, GLfloat*);
    if (!p_glGetFloatv) p_glGetFloatv = (decltype(p_glGetFloatv))GetProcAddress(GetModuleHandleA("opengl32.dll"), "glGetFloatv");
    float mv[16];
    p_glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    int hits = 0, tested = 0;
    __try {
        GLsizei picks[3] = {0, count / 2, count - 1};
        for (GLsizei k : picks) {
            unsigned idx = type == GL_UNSIGNED_SHORT ? ((const unsigned short*)indices)[k]
                         : type == GL_UNSIGNED_INT ? ((const unsigned*)indices)[k] : ((const unsigned char*)indices)[k];
            const float* v = (const float*)(g_vtxPtr + (size_t)idx * g_vtxStride);
            float pe[3], oc[3];
            for (int r = 0; r < 3; r++) pe[r] = mv[r] * v[0] + mv[4 + r] * v[1] + mv[8 + r] * v[2] + mv[12 + r];
            EyeToCam(pe, oc);
            tested++;
            if (InBodyColumn(oc)) hits++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return tested && hits * 2 > tested;  // majority of sampled vertices inside the column
}

bool VR_ShouldSkipDraw() {
    if (Features().modelCull) return false;
    if (!g_vrActive || !g_inWorldPass || !g_camPosValid) return false;
    static void(APIENTRY* p_glGetFloatv)(GLenum, GLfloat*);
    if (!p_glGetFloatv) p_glGetFloatv = (decltype(p_glGetFloatv))GetProcAddress(GetModuleHandleA("opengl32.dll"), "glGetFloatv");
    GLint mode = 0;
    float mv[16];
    p_glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    const float* oe = mv + 12;  // model origin in eye space
    float oc[3];                // ... in the camera (recenter origin) frame: Q * oe + t
    for (int r = 0; r < 3; r++) oc[r] = g_Q[r * 3 + 0] * oe[0] + g_Q[r * 3 + 1] * oe[1] + g_Q[r * 3 + 2] * oe[2] + g_t[r];
    (void)mode;
    // camera frame: +Y up, -Z forward; the body hangs below the camera hook point
    bool skip = oc[0] * oc[0] + oc[2] * oc[2] < 1.0f * 1.0f && oc[1] > -2.2f && oc[1] < 0.8f;  // body model origin sits ~0.85 m off the camera hook
    // one-frame diagnostic dump of nearby draws
    static int dumpFrame = -1;
    if (false && dumpFrame < 0 && g_worldFrames == 2300) dumpFrame = 2300;  // one-frame draw dump (diagnostics)
    if (dumpFrame == (int)g_worldFrames && oc[0] * oc[0] + oc[2] * oc[2] < 9.0f) {
        GLint prog = 0;
        static void(APIENTRY* p_gi)(GLenum, GLint*);
        if (!p_gi) p_gi = (decltype(p_gi))GetProcAddress(GetModuleHandleA("opengl32.dll"), "glGetIntegerv");
        p_gi(0x8641 /* GL_VERTEX_PROGRAM_BINDING? use ARB current */, &prog);
        g_log("bodydump: origin cam (%.2f, %.2f, %.2f) skip %d bone0 (%.2f %.2f %.2f) boneFresh %d", oc[0], oc[1], oc[2], skip,
              g_bone0[0], g_bone0[1], g_bone0[2], g_boneFresh);
    }
    g_boneFresh = 0;
    return skip;
}

static void NoteBoneParam(GLuint index, const GLfloat* v) {
    g_bone0[index - 18] = v[3];
    if (index == 20) g_boneFresh = 1;
}

// [Debug] TraceWorld=1 in kvr.ini: one GL trace of a world frame (kvr/trace_f<frame>.txt), in-game frames.
void VR_TraceNextWorldFrame() { g_traceNow = true; }

bool VR_WantTrace() {
    if (g_traceNow && g_worldThisFrame && g_vrActive) {
        g_traceNow = false;
        return true;
    }
    static int want = -1;
    if (want < 0) want = Features().traceWorld ? 1 : 0;
    static bool done;
    static int inGame;  // world frames with a player character (the menu's 3D background doesn't count)
    if (!want || done || !g_vrActive) return false;
    if (g_worldThisFrame && Weapon_PlayerCreature()) inGame++;
    if (inGame < 1500) return false;
    done = true;
    return true;
}

void VR_DrawOrigin(float out[3]) {
    static void(APIENTRY* p_glGetFloatv)(GLenum, GLfloat*);
    if (!p_glGetFloatv) p_glGetFloatv = (decltype(p_glGetFloatv))GetProcAddress(GetModuleHandleA("opengl32.dll"), "glGetFloatv");
    float mv[16];
    p_glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    for (int r = 0; r < 3; r++) out[r] = g_Q[r * 3 + 0] * mv[12] + g_Q[r * 3 + 1] * mv[13] + g_Q[r * 3 + 2] * mv[14] + g_t[r];
}
