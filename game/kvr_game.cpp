// KotOR VR - in-game layer (dinput8.dll proxy). Hooks from the Phase 0 probe + VR callbacks (vr.cpp).
// Loaded as a dinput8.dll proxy from the game folder. On the game's first
// DirectInput8Create call (SteamStub has unpacked .text by then) it:
//   1. dumps the decrypted main-module image to kvr\image_<base>.bin (private, never distributed)
//   2. patches the exe's IAT for the GL/GLU/GDI calls that carry camera state
//   3. traces every hooked call for one frame: frame 600 automatically, and on F10.

#include <windows.h>
#include <tlhelp32.h>
#include <intrin.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <cmath>

#include "vr.h"
#include "input.h"
#include "enginecam.h"
#include "combat.h"
#include "features.h"
#include "weaponmodels.h"
#include "cues.h"
#include "tutorial.h"
#include "dialogue.h"
#include "actionring.h"
#include "movie.h"
#include "ownsig.h"
#include "pick.h"
#include "simcmd.h"
#include "compat.h"
#include "animprobe.h"
#include "anim.h"

typedef HRESULT(WINAPI* DI8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

static HMODULE g_realDI8;
static DI8Create_t g_realCreate;
static char g_dir[MAX_PATH];
static FILE* g_log;
static FILE* g_trace;
static bool g_installed;
static long g_frame;
static bool g_tracing;
static int g_traceCount;
static GLenum g_mode = GL_MODELVIEW;
// own-body hiding: modelview push depth, and the depth at which a hidden node was entered (-1 = none)
static int g_mvDepth;
static int g_hideDepth = -1;
static int g_candDepth = -1;  // inside a hierarchy whose own-mesh draws are skipped (VR_IsOwnBody == 1)
// While the controlled character's body is drawn (hidden), its model matrices per stack depth, in world space: the
// bones' positions let the cull recognise models attached to them (weapons, head).
static float g_ownMat[40][16];

static void MulMat4(const float* a, const float* b, float* out) {  // column-major a * b
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            out[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1] + a[2 * 4 + r] * b[c * 4 + 2] +
                             a[3 * 4 + r] * b[c * 4 + 3];
}

void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

static void T(const char* fmt, ...) {
    if (!g_tracing || !g_trace) return;
    va_list a; va_start(a, fmt); vfprintf(g_trace, fmt, a); va_end(a);
    fputc('\n', g_trace);
}

#define FIRST(n) do { static bool once_; if (!once_) { once_ = true; Log("[f%ld] first call: %s", g_frame, n); } } while (0)

static const char* ModeName(GLenum m) {
    switch (m) {
    case GL_MODELVIEW: return "MODELVIEW";
    case GL_PROJECTION: return "PROJECTION";
    case GL_TEXTURE: return "TEXTURE";
    default: return "?";
    }
}

// ---- real function pointers (original IAT values) ----
static void(APIENTRY* r_gluPerspective)(GLdouble, GLdouble, GLdouble, GLdouble);
static void(APIENTRY* r_glMultMatrixf)(const GLfloat*);
static void(APIENTRY* r_glMatrixMode)(GLenum);
static void(APIENTRY* r_glLoadIdentity)();
static void(APIENTRY* r_glOrtho)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
static void(APIENTRY* r_glViewport)(GLint, GLint, GLsizei, GLsizei);
static void(APIENTRY* r_glTranslatef)(GLfloat, GLfloat, GLfloat);
static void(APIENTRY* r_glRotatef)(GLfloat, GLfloat, GLfloat, GLfloat);
static void(APIENTRY* r_glScalef)(GLfloat, GLfloat, GLfloat);
static void(APIENTRY* r_glPushMatrix)();
static void(APIENTRY* r_glPopMatrix)();
static void(APIENTRY* r_glClear)(GLbitfield);
static void(APIENTRY* r_glDrawElements)(GLenum, GLsizei, GLenum, const void*);
static void(APIENTRY* r_glDrawArrays)(GLenum, GLint, GLsizei);
static void(APIENTRY* r_glBegin)(GLenum);
static void(APIENTRY* r_glCallLists)(GLsizei, GLenum, const void*);
static void(APIENTRY* r_glDrawBuffer)(GLenum);
static PROC(WINAPI* r_wglGetProcAddress)(LPCSTR);
static BOOL(WINAPI* r_SwapBuffers)(HDC);

// ARB program entry points fetched via wglGetProcAddress
static void(APIENTRY* r_glBindProgramARB)(GLenum, GLuint);
static void(APIENTRY* r_glProgramEnvParameter4fARB)(GLenum, GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
static void(APIENTRY* r_glProgramEnvParameter4fvARB)(GLenum, GLuint, const GLfloat*);
static void(APIENTRY* r_glProgramLocalParameter4fARB)(GLenum, GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
static void(APIENTRY* r_glProgramLocalParameter4fvARB)(GLenum, GLuint, const GLfloat*);
static void(APIENTRY* r_glBindProgramNV)(GLenum, GLuint);
static void(APIENTRY* r_glTrackMatrixNV)(GLenum, GLuint, GLenum, GLenum);
static void(APIENTRY* r_glProgramParameter4fvNV)(GLenum, GLuint, const GLfloat*);

// ---- hooks ----
static void APIENTRY h_gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zn, GLdouble zf) { FIRST("gluPerspective");
    static double seen[64][2]; static int nseen;
    bool known = false;
    for (int i = 0; i < nseen; i++) known |= (seen[i][0] == fovy && fabs(seen[i][1] - aspect) < 0.01);
    if (!known && nseen < 64) {
        seen[nseen][0] = fovy; seen[nseen][1] = aspect; nseen++;
        Log("[f%ld] gluPerspective fovy=%.3f aspect=%.4f near=%.4f far=%.2f", g_frame, fovy, aspect, zn, zf);
    }
    T("gluPerspective %.3f %.4f %.4f %.2f", fovy, aspect, zn, zf);
    if (VR_OnPerspective(fovy, aspect, zn, zf)) { T("  -> VR eye frustum"); return; }
    r_gluPerspective(fovy, aspect, zn, zf);
}
static void APIENTRY h_glMultMatrixf(const GLfloat* m) { FIRST("glMultMatrixf");
    if (g_mode == GL_PROJECTION) {
        static float seen[32][2]; static int nseen;
        bool known = false;
        for (int i = 0; i < nseen; i++) known |= (fabsf(seen[i][0] - m[0]) < 1e-3f && fabsf(seen[i][1] - m[5]) < 1e-3f);
        if (!known && nseen < 32) {
            seen[nseen][0] = m[0]; seen[nseen][1] = m[5]; nseen++;
            Log("[f%ld] PROJECTION glMultMatrixf %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f  (vfov=%.2f deg)",
                g_frame, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15],
                m[5] != 0 ? 2.0 * atan(1.0 / m[5]) * 57.29578 : 0.0);
        }
    }
    T("glMultMatrixf [%s] %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
      ModeName(g_mode), m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    if (VR_OnMultMatrixf(g_mode, m)) { T("  -> VR eye view"); return; }
    if (g_mode == GL_MODELVIEW && g_hideDepth >= 0 && g_mvDepth > g_hideDepth && g_mvDepth < 40) {
        float t[16];  // a bone inside the hidden body: track its world transform
        MulMat4(g_ownMat[g_mvDepth], m, t);
        memcpy(g_ownMat[g_mvDepth], t, sizeof t);
        VR_OwnNode(t + 12);
    }
    // (a pure rotation is the camera's view, never a model root: with you lying in the opening's bunk the eye sat on
    // your body's root and the view itself was hidden as "your body" - the whole world went black, 2026-11-05)
    bool pureRotation = m[12] == 0.0f && m[13] == 0.0f && m[14] == 0.0f;
    if (g_mode == GL_MODELVIEW && g_hideDepth < 0 && !pureRotation) {
        int own = VR_IsOwnBody(m + 12, g_mvDepth);
        if (own == 2) {
            g_hideDepth = g_mvDepth;
            if (g_mvDepth < 40) memcpy(g_ownMat[g_mvDepth], m, sizeof g_ownMat[0]);
            T("  -> own body, hidden");
        }
        else if (own == 1 && g_candDepth < 0) { g_candDepth = g_mvDepth; T("  -> near the player: own meshes hidden"); }
    }
    r_glMultMatrixf(m);
}
static void APIENTRY h_glMatrixMode(GLenum m) { FIRST("glMatrixMode"); g_mode = m; T("glMatrixMode %s", ModeName(m)); r_glMatrixMode(m); }
static void APIENTRY h_glLoadIdentity() { FIRST("glLoadIdentity"); T("glLoadIdentity [%s]", ModeName(g_mode)); r_glLoadIdentity(); }
static void APIENTRY h_glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) { FIRST("glOrtho");
    T("glOrtho %.1f %.1f %.1f %.1f %.2f %.2f", l, r, b, t, n, f); VR_OnOrtho(l, r, b, t); r_glOrtho(l, r, b, t, n, f);
}
static void APIENTRY h_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { FIRST("glViewport"); T("glViewport %d %d %d %d", x, y, w, h); r_glViewport(x, y, w, h); }
static void APIENTRY h_glTranslatef(GLfloat x, GLfloat y, GLfloat z) { FIRST("glTranslatef"); T("glTranslatef [%s] %.4f %.4f %.4f", ModeName(g_mode), x, y, z); if (VR_OnTranslatef(g_mode, x, y, z)) { T("  -> VR eye view"); return; } r_glTranslatef(x, y, z); }
static void APIENTRY h_glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z) { FIRST("glRotatef"); T("glRotatef [%s] %.3f (%.3f %.3f %.3f)", ModeName(g_mode), a, x, y, z); r_glRotatef(a, x, y, z); }
static void APIENTRY h_glScalef(GLfloat x, GLfloat y, GLfloat z) { FIRST("glScalef"); T("glScalef [%s] %.4f %.4f %.4f", ModeName(g_mode), x, y, z); r_glScalef(x, y, z); }
static void APIENTRY h_glPushMatrix() { FIRST("glPushMatrix"); T("glPushMatrix [%s]", ModeName(g_mode)); if (g_mode == GL_MODELVIEW) { g_mvDepth++; if (g_hideDepth >= 0 && g_mvDepth > g_hideDepth && g_mvDepth < 40) memcpy(g_ownMat[g_mvDepth], g_ownMat[g_mvDepth - 1], sizeof g_ownMat[0]); } r_glPushMatrix(); }
static void APIENTRY h_glPopMatrix() { FIRST("glPopMatrix"); T("glPopMatrix [%s]", ModeName(g_mode)); if (g_mode == GL_MODELVIEW) { g_mvDepth--; if (g_hideDepth >= 0 && g_mvDepth < g_hideDepth) g_hideDepth = -1; if (g_candDepth >= 0 && g_mvDepth < g_candDepth) g_candDepth = -1; } r_glPopMatrix(); }
static void APIENTRY h_glClear(GLbitfield b) { FIRST("glClear"); T("glClear 0x%x", b); r_glClear(b); }
static void APIENTRY h_glDrawBuffer(GLenum b) { FIRST("glDrawBuffer"); T("glDrawBuffer 0x%x", b); r_glDrawBuffer(b); }
static void APIENTRY h_glDrawElements(GLenum m, GLsizei c, GLenum t, const void* i) { FIRST("glDrawElements"); if (g_hideDepth >= 0 || (g_candDepth >= 0 && OwnSig_Match(c)) || VR_ShouldSkipDraw() || VR_ShouldSkipElements(c, t, i) || VR_InsideHead(c, t, i)) return; if (g_tracing) g_traceCount++; if (g_tracing) { float oc[3]; VR_DrawOrigin(oc); T("  glDrawElements mode=%u count=%d  origin_cam (%.2f %.2f %.2f)", m, c, oc[0], oc[1], oc[2]); } if (VR_NoteDrawn(c, t, i) && g_tracing) T("  ^^ this draw comes within 12 cm of the eye"); r_glDrawElements(m, c, t, i); }
static void APIENTRY h_glDrawArrays(GLenum m, GLint f, GLsizei c) { FIRST("glDrawArrays"); if (g_hideDepth >= 0 || VR_ShouldSkipDraw()) return; if (g_tracing) g_traceCount++; T("  glDrawArrays mode=%u first=%d count=%d", m, f, c); r_glDrawArrays(m, f, c); }
// Immediate-mode draws can't be skipped (the glVertex calls that follow would error), so an own-body glBegin
// block draws with colour and depth writes masked off until its glEnd.
static void(APIENTRY* r_glEnd)();
static void(APIENTRY* p_glColorMaskReal)(GLboolean, GLboolean, GLboolean, GLboolean);
static void(APIENTRY* p_glDepthMaskReal)(GLboolean);
static void(APIENTRY* p_glGetBooleanvReal)(GLenum, GLboolean*);
static bool g_beginMasked;
static GLboolean g_savedColorMask[4], g_savedDepthMask;
static void APIENTRY h_glBegin(GLenum m) {
    FIRST("glBegin");
    if (g_tracing) g_traceCount++;
    if (g_hideDepth >= 0 || VR_ShouldSkipDraw()) {
        if (!p_glColorMaskReal) {
            HMODULE gl = GetModuleHandleA("opengl32.dll");
            p_glColorMaskReal = (decltype(p_glColorMaskReal))GetProcAddress(gl, "glColorMask");
            p_glDepthMaskReal = (decltype(p_glDepthMaskReal))GetProcAddress(gl, "glDepthMask");
            p_glGetBooleanvReal = (decltype(p_glGetBooleanvReal))GetProcAddress(gl, "glGetBooleanv");
        }
        p_glGetBooleanvReal(GL_COLOR_WRITEMASK, g_savedColorMask);
        p_glGetBooleanvReal(GL_DEPTH_WRITEMASK, &g_savedDepthMask);
        p_glColorMaskReal(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        p_glDepthMaskReal(GL_FALSE);
        g_beginMasked = true;
        T("  glBegin %u  (own body, masked)", m);
    } else {
        T("  glBegin %u", m);
    }
    r_glBegin(m);
}
static void APIENTRY h_glEnd() {
    r_glEnd();
    if (g_beginMasked) {
        g_beginMasked = false;
        p_glColorMaskReal(g_savedColorMask[0], g_savedColorMask[1], g_savedColorMask[2], g_savedColorMask[3]);
        p_glDepthMaskReal(g_savedDepthMask);
    }
}
static void APIENTRY h_glCallLists(GLsizei n, GLenum t, const void* l) { FIRST("glCallLists"); T("  glCallLists %d", n); r_glCallLists(n, t, l); }

static void APIENTRY h_glBindProgramARB(GLenum t, GLuint p) { T("glBindProgramARB 0x%x %u", t, p); r_glBindProgramARB(t, p); }
static void APIENTRY h_glProgramEnvParameter4fARB(GLenum t, GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    GLfloat v[4] = {x, y, z, w};
    if (VR_OnEnvParam(t, i, v)) T("  -> VR env [%u]", i);
    T("glProgramEnvParameter4fARB 0x%x [%u] %.4f %.4f %.4f %.4f", t, i, v[0], v[1], v[2], v[3]); r_glProgramEnvParameter4fARB(t, i, v[0], v[1], v[2], v[3]);
}
static void APIENTRY h_glProgramEnvParameter4fvARB(GLenum t, GLuint i, const GLfloat* v) {
    GLfloat c[4] = {v[0], v[1], v[2], v[3]};
    VR_OnEnvParam(t, i, c);
    T("glProgramEnvParameter4fvARB 0x%x [%u] %.4f %.4f %.4f %.4f", t, i, c[0], c[1], c[2], c[3]); r_glProgramEnvParameter4fvARB(t, i, c);
}
static void APIENTRY h_glProgramLocalParameter4fARB(GLenum t, GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    T("glProgramLocalParameter4fARB 0x%x [%u] %.4f %.4f %.4f %.4f", t, i, x, y, z, w); r_glProgramLocalParameter4fARB(t, i, x, y, z, w);
}
static void APIENTRY h_glProgramLocalParameter4fvARB(GLenum t, GLuint i, const GLfloat* v) {
    T("glProgramLocalParameter4fvARB 0x%x [%u] %.4f %.4f %.4f %.4f", t, i, v[0], v[1], v[2], v[3]); r_glProgramLocalParameter4fvARB(t, i, v);
}
static void APIENTRY h_glBindProgramNV(GLenum t, GLuint p) { T("glBindProgramNV 0x%x %u", t, p); r_glBindProgramNV(t, p); }
static void APIENTRY h_glTrackMatrixNV(GLenum t, GLuint a, GLenum m, GLenum x) { T("glTrackMatrixNV 0x%x %u 0x%x 0x%x", t, a, m, x); r_glTrackMatrixNV(t, a, m, x); }
static void APIENTRY h_glProgramParameter4fvNV(GLenum t, GLuint i, const GLfloat* v) {
    T("glProgramParameter4fvNV 0x%x [%u] %.4f %.4f %.4f %.4f", t, i, v[0], v[1], v[2], v[3]); r_glProgramParameter4fvNV(t, i, v);
}

// texture-name protection for the VR export textures, and context-switch logging
static void(APIENTRY* r_glDeleteTextures)(GLsizei, const GLuint*);
static void(APIENTRY* r_glBindTexture)(GLenum, GLuint);
static BOOL(WINAPI* r_wglMakeCurrent)(HDC, HGLRC);
static void APIENTRY h_glDeleteTextures(GLsizei n, const GLuint* names) {
    GLuint keep[256];
    if (n > 0 && n <= 256) {
        GLsizei k = 0;
        for (GLsizei i = 0; i < n; i++) {
            if (VR_IsOurTexture(names[i]) && !VR_InCapture()) { Log("[f%ld] game tried to delete VR texture %u - blocked", g_frame, names[i]); continue; }
            keep[k++] = names[i];
        }
        if (k) r_glDeleteTextures(k, keep);
        return;
    }
    r_glDeleteTextures(n, names);
}
static void APIENTRY h_glBindTexture(GLenum t, GLuint name) {
    if (!VR_InCapture() && VR_IsOurTexture(name)) {
        static int c;
        if (c++ < 20) Log("[f%ld] game bound VR texture name %u (collision)", g_frame, name);
    }
    r_glBindTexture(t, name);
}
static BOOL WINAPI h_wglMakeCurrent(HDC dc, HGLRC rc) {
    static int c;
    if (c++ < 12) Log("[f%ld] wglMakeCurrent dc=%p rc=%p", g_frame, dc, rc);
    return r_wglMakeCurrent(dc, rc);
}

static BOOL(WINAPI* r_wglShareLists)(HGLRC, HGLRC);
static BOOL WINAPI h_wglShareLists(HGLRC a, HGLRC b) {
    static int c;
    if (c++ < 10) Log("[f%ld] wglShareLists %p -> %p", g_frame, a, b);
    VR_BeforeShareLists();
    return r_wglShareLists(a, b);
}

// context / pbuffer churn counters (loading-screen thread leak investigation)
static volatile LONG g_nCtxCreate, g_nCtxDelete, g_nPbCreate, g_nPbDestroy, g_nPbLost;
static HGLRC(WINAPI* r_wglCreateContext)(HDC);
static BOOL(WINAPI* r_wglDeleteContext)(HGLRC);
static HGLRC WINAPI h_wglCreateContext(HDC dc) {
    InterlockedIncrement(&g_nCtxCreate);
    void* ret = _ReturnAddress();
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ret, &m))
        GetModuleFileNameA(m, name, MAX_PATH);
    // Discord's overlay creates a new context per frame on the game's pbuffer DCs and never deletes them;
    // each NVIDIA context brings ~28 driver threads, so the game starves within seconds. Refusing makes
    // DiscordHook abort(), so hand it back one cached context per DC instead.
    if (strstr(name, "DiscordHook")) {
        static HDC cacheDc[32];
        static HGLRC cacheRc[32];
        for (int i = 0; i < 32; i++)
            if (cacheDc[i] == dc && cacheRc[i]) return cacheRc[i];
        HGLRC rc = r_wglCreateContext(dc);
        for (int i = 0; i < 32; i++)
            if (!cacheRc[i]) { cacheDc[i] = dc; cacheRc[i] = rc; break; }
        Log("[f%ld] DiscordHook context for dc %p cached -> %p", g_frame, dc, rc);
        return rc;
    }
    HGLRC rc = r_wglCreateContext(dc);
    static int c;
    if (c++ < 40) Log("[f%ld] wglCreateContext dc=%p -> %p from %p (%s) thread %lu", g_frame, dc, rc, ret, strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name, GetCurrentThreadId());
    return rc;
}
static BOOL WINAPI h_wglDeleteContext(HGLRC rc) { InterlockedIncrement(&g_nCtxDelete); return r_wglDeleteContext(rc); }
static void*(WINAPI* r_wglCreatePbufferARB)(HDC, int, int, int, const int*);
static BOOL(WINAPI* r_wglDestroyPbufferARB)(void*);
static BOOL(WINAPI* r_wglQueryPbufferARB)(void*, int, int*);
static void* WINAPI h_wglCreatePbufferARB(HDC dc, int fmt, int w, int h, const int* attr) {
    InterlockedIncrement(&g_nPbCreate);
    static int c;
    if (c++ < 20) Log("[f%ld] wglCreatePbufferARB %dx%d fmt %d", g_frame, w, h, fmt);
    return r_wglCreatePbufferARB(dc, fmt, w, h, attr);
}
static BOOL WINAPI h_wglDestroyPbufferARB(void* pb) { InterlockedIncrement(&g_nPbDestroy); return r_wglDestroyPbufferARB(pb); }
static BOOL WINAPI h_wglQueryPbufferARB(void* pb, int attrib, int* value) {
    BOOL r = r_wglQueryPbufferARB(pb, attrib, value);
    if (r && attrib == 0x2033 /* WGL_PBUFFER_LOST_ARB */ && value && *value) InterlockedIncrement(&g_nPbLost);
    return r;
}

static int CountThreads() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    THREADENTRY32 te{sizeof te};
    int n = 0;
    DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid) n++;
    CloseHandle(snap);
    return n;
}

static BOOL(WINAPI* r_GetCursorInfo)(PCURSORINFO);
static BOOL WINAPI h_GetCursorInfo(PCURSORINFO ci) {
    BOOL r = r_GetCursorInfo(ci);
    if (r && ci) Input_OnGameGetCursorInfo(ci);
    return r;
}

static BOOL(WINAPI* r_SetCursorPos)(int, int);
static BOOL WINAPI h_SetCursorPos(int x, int y) {
    Input_OnGameSetCursorPos(x, y);
    return r_SetCursorPos(x, y);
}

static void(APIENTRY* r_glVertexPointer)(GLint, GLenum, GLsizei, const void*);
static void APIENTRY h_glVertexPointer(GLint size, GLenum type, GLsizei stride, const void* ptr) {
    VR_OnVertexPointer(size, type, stride, ptr);
    r_glVertexPointer(size, type, stride, ptr);
}

struct ProcHook { const char* name; void** real; void* hook; };
static ProcHook g_procHooks[] = {
    {"wglCreatePbufferARB", (void**)&r_wglCreatePbufferARB, (void*)h_wglCreatePbufferARB},
    {"wglDestroyPbufferARB", (void**)&r_wglDestroyPbufferARB, (void*)h_wglDestroyPbufferARB},
    {"wglQueryPbufferARB", (void**)&r_wglQueryPbufferARB, (void*)h_wglQueryPbufferARB},
    {"glBindProgramARB", (void**)&r_glBindProgramARB, (void*)h_glBindProgramARB},
    {"glProgramEnvParameter4fARB", (void**)&r_glProgramEnvParameter4fARB, (void*)h_glProgramEnvParameter4fARB},
    {"glProgramEnvParameter4fvARB", (void**)&r_glProgramEnvParameter4fvARB, (void*)h_glProgramEnvParameter4fvARB},
    {"glProgramLocalParameter4fARB", (void**)&r_glProgramLocalParameter4fARB, (void*)h_glProgramLocalParameter4fARB},
    {"glProgramLocalParameter4fvARB", (void**)&r_glProgramLocalParameter4fvARB, (void*)h_glProgramLocalParameter4fvARB},
    {"glBindProgramNV", (void**)&r_glBindProgramNV, (void*)h_glBindProgramNV},
    {"glTrackMatrixNV", (void**)&r_glTrackMatrixNV, (void*)h_glTrackMatrixNV},
    {"glProgramParameter4fvNV", (void**)&r_glProgramParameter4fvNV, (void*)h_glProgramParameter4fvNV},
};

static PROC WINAPI h_wglGetProcAddress(LPCSTR name) {
    PROC p = r_wglGetProcAddress(name);
    Log("wglGetProcAddress %s -> %p", name, p);
    if (p) {
        for (auto& h : g_procHooks) {
            if (strcmp(name, h.name) == 0) { *h.real = (void*)p; return (PROC)h.hook; }
        }
    }
    return p;
}

// The game sets this flag around its own SwapBuffers call (0x4048AE / 0x4048D5); the movie player thread waits for
// it to read 0 before it shows its window (0x405875). Our VR work below runs inside that window, so left alone the
// flag reads 1 nearly all the time: queued cutscene movies (the opening crawl) start minutes late and can stall
// unshown. Keep it clear during our work and set it again only around the real swap.
static volatile LONG* const kInSwapFlag = (volatile LONG*)0x7A3C9C;

static BOOL WINAPI h_SwapBuffers(HDC dc) {
    LONG inSwap = *kInSwapFlag;
    *kInSwapFlag = 0;
    FIRST("wglSwapBuffers");
    if (g_frame % 1800 == 0) Log("heartbeat frame %ld", g_frame);
    if (g_tracing) {
        T("==== SwapBuffers (frame %ld, %d draws) ====", g_frame, g_traceCount);
        fclose(g_trace); g_trace = nullptr; g_tracing = false;
        Log("trace for frame %ld written", g_frame);
    }
    g_frame++;
    static bool f10Down;
    bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if ((g_frame == 600 || g_frame == 3600 || (f10 && !f10Down) || VR_WantTrace()) && !g_tracing) {
        char path[MAX_PATH];
        sprintf_s(path, "%s\\kvr\\trace_f%ld.txt", g_dir, g_frame);
        g_trace = _fsopen(path, "w", _SH_DENYNO);
        if (g_trace) { g_tracing = true; g_traceCount = 0; }
    }
    f10Down = f10;
    AnimProbe_OnSwap();
    VR_OnSwap(dc);
    g_mvDepth = 0;
    g_hideDepth = -1;
    g_candDepth = -1;
    Input_Update(WindowFromDC(dc));
    static DWORD lastChurn;
    if (GetTickCount() - lastChurn > 1000) {
        lastChurn = GetTickCount();
        Log("[f%ld t=%lu] churn: ctx +%ld -%ld, pbuf +%ld -%ld lost %ld, threads %d", g_frame, GetTickCount(), g_nCtxCreate, g_nCtxDelete,
            g_nPbCreate, g_nPbDestroy, g_nPbLost, CountThreads());
    }
    *kInSwapFlag = inSwap;
    return r_SwapBuffers(dc);
}

// ---- hooking: hot-patch the system export, IAT as fallback ----
struct IatHook { const char* dll; const char* func; void** real; void* hook; };
static IatHook g_iatHooks[] = {
    {"GLU32.dll", "gluPerspective", (void**)&r_gluPerspective, (void*)h_gluPerspective},
    {"OPENGL32.dll", "glMultMatrixf", (void**)&r_glMultMatrixf, (void*)h_glMultMatrixf},
    {"OPENGL32.dll", "glMatrixMode", (void**)&r_glMatrixMode, (void*)h_glMatrixMode},
    {"OPENGL32.dll", "glLoadIdentity", (void**)&r_glLoadIdentity, (void*)h_glLoadIdentity},
    {"OPENGL32.dll", "glOrtho", (void**)&r_glOrtho, (void*)h_glOrtho},
    {"OPENGL32.dll", "glViewport", (void**)&r_glViewport, (void*)h_glViewport},
    {"OPENGL32.dll", "glTranslatef", (void**)&r_glTranslatef, (void*)h_glTranslatef},
    {"OPENGL32.dll", "glRotatef", (void**)&r_glRotatef, (void*)h_glRotatef},
    {"OPENGL32.dll", "glScalef", (void**)&r_glScalef, (void*)h_glScalef},
    {"OPENGL32.dll", "glPushMatrix", (void**)&r_glPushMatrix, (void*)h_glPushMatrix},
    {"OPENGL32.dll", "glPopMatrix", (void**)&r_glPopMatrix, (void*)h_glPopMatrix},
    {"OPENGL32.dll", "glClear", (void**)&r_glClear, (void*)h_glClear},
    {"OPENGL32.dll", "glDrawBuffer", (void**)&r_glDrawBuffer, (void*)h_glDrawBuffer},
    {"OPENGL32.dll", "glDrawElements", (void**)&r_glDrawElements, (void*)h_glDrawElements},
    {"OPENGL32.dll", "glDrawArrays", (void**)&r_glDrawArrays, (void*)h_glDrawArrays},
    {"OPENGL32.dll", "glBegin", (void**)&r_glBegin, (void*)h_glBegin},
    {"OPENGL32.dll", "glEnd", (void**)&r_glEnd, (void*)h_glEnd},
    {"OPENGL32.dll", "glVertexPointer", (void**)&r_glVertexPointer, (void*)h_glVertexPointer},
    {"OPENGL32.dll", "glCallLists", (void**)&r_glCallLists, (void*)h_glCallLists},
    {"OPENGL32.dll", "wglGetProcAddress", (void**)&r_wglGetProcAddress, (void*)h_wglGetProcAddress},
    {"OPENGL32.dll", "glDeleteTextures", (void**)&r_glDeleteTextures, (void*)h_glDeleteTextures},
    {"OPENGL32.dll", "glBindTexture", (void**)&r_glBindTexture, (void*)h_glBindTexture},
    {"OPENGL32.dll", "wglMakeCurrent", (void**)&r_wglMakeCurrent, (void*)h_wglMakeCurrent},
    {"OPENGL32.dll", "wglShareLists", (void**)&r_wglShareLists, (void*)h_wglShareLists},
    {"USER32.dll", "SetCursorPos", (void**)&r_SetCursorPos, (void*)h_SetCursorPos},
    {"USER32.dll", "GetCursorInfo", (void**)&r_GetCursorInfo, (void*)h_GetCursorInfo},
    {"OPENGL32.dll", "wglCreateContext", (void**)&r_wglCreateContext, (void*)h_wglCreateContext},
    {"OPENGL32.dll", "wglDeleteContext", (void**)&r_wglDeleteContext, (void*)h_wglDeleteContext},
};

static void PatchIat(HMODULE mod) {
    auto base = (BYTE*)mod;
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        const char* dll = (const char*)(base + imp->Name);
        if (!imp->OriginalFirstThunk) { Log("no OriginalFirstThunk for %s", dll); continue; }
        auto names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        auto slots = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const char* fn = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
            for (auto& h : g_iatHooks) {
                if (!h.hook || _stricmp(dll, h.dll) || strcmp(fn, h.func)) continue;
                DWORD old;
                VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
                *h.real = (void*)slots->u1.Function;
                slots->u1.Function = (ULONG_PTR)h.hook;
                VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
                Log("IAT %s!%s -> hooked (real %p)", dll, fn, *h.real);
            }
        }
    }
    for (auto& h : g_iatHooks)
        if (!*h.real) Log("WARNING: %s!%s not found in IAT", h.dll, h.func);
}


// System DLL exports built with /hotpatch start with `mov edi, edi` and have 5 bytes of
// int3/nop padding before them. Write `jmp hook` into the padding and a short jump back
// over the padding at the entry; the trampoline is entry+2.
static bool HotPatch(void* target, void* hook, void** tramp) {
    BYTE* p = (BYTE*)target;
    if (p[0] != 0x8B || p[1] != 0xFF) return false;
    for (int i = 1; i <= 5; i++)
        if (p[-i] != 0xCC && p[-i] != 0x90) return false;
    DWORD old;
    if (!VirtualProtect(p - 5, 7, PAGE_EXECUTE_READWRITE, &old)) return false;
    p[-5] = 0xE9;
    *(INT32*)(p - 4) = (INT32)((BYTE*)hook - p);
    *(WORD*)p = 0xF9EB;  // jmp short -7
    VirtualProtect(p - 5, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p - 5, 7);
    *tramp = p + 2;
    return true;
}

// GDI32!SwapBuffers is `jmp dword ptr [p]` into gdi32full. Swap *p: sits below any overlay detour
// on opengl32!wglSwapBuffers and catches every caller.
static bool HookGdiSwapBuffers() {
    BYTE* p = (BYTE*)GetProcAddress(GetModuleHandleA("GDI32.dll"), "SwapBuffers");
    if (!p) return false;
    if (p[0] != 0xFF || p[1] != 0x25) {
        Log("GDI32!SwapBuffers prologue %02X %02X %02X %02X %02X %02X - not a jmp [ptr] stub", p[0], p[1], p[2], p[3], p[4], p[5]);
        return false;
    }
    void** slot = *(void***)(p + 2);
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    r_SwapBuffers = (BOOL(WINAPI*)(HDC))*slot;
    *slot = (void*)h_SwapBuffers;
    VirtualProtect(slot, sizeof(void*), old, &old);
    Log("GDI32!SwapBuffers pointer hooked (real %p)", r_SwapBuffers);
    return true;
}

static void InstallHooks(HMODULE exe) {
    Log("GameOverlayRenderer loaded: %s", GetModuleHandleA("GameOverlayRenderer.dll") ? "yes" : "no");
    HookGdiSwapBuffers();
    for (auto& h : g_iatHooks) {
        HMODULE m = GetModuleHandleA(h.dll);
        void* fn = m ? (void*)GetProcAddress(m, h.func) : nullptr;
        if (fn && HotPatch(fn, h.hook, h.real)) { Log("hotpatch %s!%s ok", h.dll, h.func); h.hook = nullptr; }
        else if (fn) { BYTE* b = (BYTE*)fn; Log("no hotpatch %s!%s: %02X %02X %02X %02X %02X", h.dll, h.func, b[0], b[1], b[2], b[3], b[4]); }
    }
    PatchIat(exe);  // whatever could not be hot-patched (h.hook still set)
}

static void DumpImage(HMODULE mod) {
    auto base = (BYTE*)mod;
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    DWORD size = nt->OptionalHeader.SizeOfImage;
    char path[MAX_PATH];
    sprintf_s(path, "%s\\kvr\\image_%08X.bin", g_dir, (unsigned)(ULONG_PTR)base);
    FILE* f;
    if (fopen_s(&f, path, "wb")) { Log("dump: cannot open %s", path); return; }
    static BYTE zero[4096];
    for (DWORD off = 0; off < size; off += 4096) {
        MEMORY_BASIC_INFORMATION mbi;
        bool ok = VirtualQuery(base + off, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
                  !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        fwrite(ok ? base + off : zero, 1, 4096, f);
    }
    fclose(f);
    Log("dumped image base=%p size=0x%X to %s", base, size, path);
}

static void Install() {
    if (g_installed) return;
    g_installed = true;
    HMODULE exe = GetModuleHandleA(nullptr);
    char dump[MAX_PATH];
    sprintf_s(dump, "%s\\kvr\\image_%08X.bin", g_dir, (unsigned)(ULONG_PTR)exe);
    if (GetFileAttributesA(dump) == INVALID_FILE_ATTRIBUTES) DumpImage(exe);
    InstallHooks(exe);
    VrGl gl{};
    gl.MultMatrixf = r_glMultMatrixf;
    gl.Translatef = r_glTranslatef;
    gl.Frustum = (void(APIENTRY*)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble))GetProcAddress(GetModuleHandleA("opengl32.dll"), "glFrustum");
    gl.GetProcAddress = r_wglGetProcAddress;
    VR_Init(gl, Log);
    Features_Load(g_dir);
    {
        char ini[MAX_PATH];
        sprintf_s(ini, "%s\\kvr\\kvr.ini", g_dir);
        Compat_Install(GetPrivateProfileIntA("Compat", "DiscordOverlay", 0, ini) != 0);
    }
    WeaponModels_Init(g_dir);
    Cues_Init(g_dir);
    Tutorial_Init(g_dir);
    Dialogue_Install();
    ActionRing_Init(g_dir);
    Movie_Install();
    OwnSig_Init(g_dir);
    Pick_Init(g_dir);
    SimCmd_Init(g_dir);
    if (Features().engineCamera) EngineCam_Install();
    if (Features().combat && Combat_Install()) Anim_Install();
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    Install();
    HRESULT hr = g_realCreate ? g_realCreate(inst, ver, riid, out, outer) : E_FAIL;
    static const GUID IID_DI8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(riid, IID_DI8A)) {
        *out = Input_WrapDirectInput(*out);
        Log("input: IDirectInput8A wrapped");
    }
    return hr;
}

// ---- early code patches -----------------------------------------------------------------------------
// The resolution whitelist is read during startup, before DirectInput8Create. SteamStub decrypts .text after
// our DLL loads, so a thread waits for the original bytes to appear, then patches them.
struct CodePatch { DWORD va; const char* what; BYTE orig[12]; int origLen; BYTE repl[12]; int replLen; };
static CodePatch g_earlyPatches[] = {
    // 0x5F0C60: resolution validator (800x600, 1024x768, 1280x960/1024, 1600x1200 only) -> return 1
    {0x5F0C60, "resolution whitelist", {0x8B, 0x44, 0x24, 0x04, 0x3D, 0x20, 0x03, 0x00, 0x00}, 9,
     {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC2, 0x08, 0x00}, 8},
};

static DWORD WINAPI EarlyPatchThread(LPVOID) {
    DWORD start = GetTickCount();
    int pending = (int)(sizeof g_earlyPatches / sizeof g_earlyPatches[0]);
    bool done[8] = {};
    while (pending > 0 && GetTickCount() - start < 30000) {
        for (int i = 0; i < (int)(sizeof g_earlyPatches / sizeof g_earlyPatches[0]); i++) {
            if (done[i]) continue;
            CodePatch& p = g_earlyPatches[i];
            BYTE* at = (BYTE*)(ULONG_PTR)p.va;
            __try {
                if (memcmp(at, p.orig, p.origLen) != 0) continue;
            } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            DWORD old;
            VirtualProtect(at, p.replLen, PAGE_EXECUTE_READWRITE, &old);
            memcpy(at, p.repl, p.replLen);
            VirtualProtect(at, p.replLen, old, &old);
            FlushInstructionCache(GetCurrentProcess(), at, p.replLen);
            done[i] = true;
            pending--;
            Log("early patch applied: %s at 0x%08X after %lu ms", p.what, p.va, GetTickCount() - start);
        }
        if (pending > 0) SwitchToThread();
    }
    if (pending > 0) Log("WARNING: %d early patch(es) not applied (bytes never matched)", pending);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        GetModuleFileNameA(self, g_dir, MAX_PATH);
        *strrchr(g_dir, '\\') = 0;
        char path[MAX_PATH];
        sprintf_s(path, "%s\\kvr", g_dir);
        CreateDirectoryA(path, nullptr);
        sprintf_s(path, "%s\\kvr\\probe.log", g_dir);
        g_log = _fsopen(path, "w", _SH_DENYNO);
        char sys[MAX_PATH];
        GetSystemDirectoryA(sys, MAX_PATH);  // SysWOW64 for a 32-bit process
        strcat_s(sys, "\\dinput8.dll");
        g_realDI8 = LoadLibraryA(sys);
        g_realCreate = g_realDI8 ? (DI8Create_t)GetProcAddress(g_realDI8, "DirectInput8Create") : nullptr;
        Log("kvr probe loaded; real dinput8=%p create=%p", g_realDI8, g_realCreate);
        CloseHandle(CreateThread(nullptr, 0, EarlyPatchThread, nullptr, 0, nullptr));
    }
    return TRUE;
}
