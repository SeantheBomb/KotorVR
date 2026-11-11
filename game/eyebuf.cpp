// The eye buffer (see eyebuf.h).
//
// Each eye used to be exactly KotOR's window: 1440x1080 at best on a 1440p monitor at 125% scaling, and 4:3 while a
// headset's per-eye view is taller than wide - about half the vertical detail a Quest 3 wants (2026-11-10). Now the
// world pass draws into an EXT_framebuffer_object at the host's recommended per-eye size, multisampled (resolved
// before any read) with a packed depth-stencil (KotOR's shadows are stencil volumes). The engine never knows: its
// full-window viewport covers the whole buffer and its back-buffer draw targets mean the colour attachment. At the
// end of the world pass a scaled copy goes into the window, so everything after it - the monitor mirror, glow,
// the GUI - sees what it always did.
#include "eyebuf.h"

extern void Log(const char* fmt, ...);

#define GL_FRAMEBUFFER_EXT 0x8D40
#define GL_READ_FRAMEBUFFER_EXT 0x8CA8
#define GL_DRAW_FRAMEBUFFER_EXT 0x8CA9
#define GL_RENDERBUFFER_EXT 0x8D41
#define GL_COLOR_ATTACHMENT0_EXT 0x8CE0
#define GL_DEPTH_ATTACHMENT_EXT 0x8D00
#define GL_STENCIL_ATTACHMENT_EXT 0x8D20
#define GL_DEPTH24_STENCIL8_EXT 0x88F0
#define GL_FRAMEBUFFER_COMPLETE_EXT 0x8CD5
#define GL_MAX_SAMPLES_EXT 0x8D57
#define GL_TEXTURE0_ARB_ 0x84C0
#define GL_CLAMP_TO_EDGE_ 0x812F
#define GL_VERTEX_PROGRAM_ARB_ 0x8620
#define GL_FRAGMENT_PROGRAM_ARB_ 0x8804
#define GL_REGISTER_COMBINERS_NV_ 0x8522
#define GL_TEXTURE_SHADER_NV_ 0x86DE

namespace {

struct Gl {
    void(APIENTRY* GenFramebuffers)(GLsizei, GLuint*);
    void(APIENTRY* DeleteFramebuffers)(GLsizei, const GLuint*);
    void(APIENTRY* BindFramebuffer)(GLenum, GLuint);
    void(APIENTRY* GenRenderbuffers)(GLsizei, GLuint*);
    void(APIENTRY* DeleteRenderbuffers)(GLsizei, const GLuint*);
    void(APIENTRY* BindRenderbuffer)(GLenum, GLuint);
    void(APIENTRY* RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
    void(APIENTRY* RenderbufferStorageMultisample)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);  // may be missing
    void(APIENTRY* FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
    void(APIENTRY* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum(APIENTRY* CheckFramebufferStatus)(GLenum);
    void(APIENTRY* BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    void(APIENTRY* ActiveTexture)(GLenum);
    // core 1.1 (opengl32 exports: through our hot-patches, so the hooks see these calls too)
    void(APIENTRY* Viewport)(GLint, GLint, GLsizei, GLsizei);
    void(APIENTRY* GetIntegerv)(GLenum, GLint*);
    void(APIENTRY* Clear)(GLbitfield);
    void(APIENTRY* GenTextures)(GLsizei, GLuint*);
    void(APIENTRY* DeleteTextures)(GLsizei, const GLuint*);
    void(APIENTRY* BindTexture)(GLenum, GLuint);
    void(APIENTRY* TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
    void(APIENTRY* TexParameteri)(GLenum, GLenum, GLint);
    void(APIENTRY* PushAttrib)(GLbitfield);
    void(APIENTRY* PopAttrib)();
    void(APIENTRY* MatrixMode)(GLenum);
    void(APIENTRY* PushMatrix)();
    void(APIENTRY* PopMatrix)();
    void(APIENTRY* LoadIdentity)();
    void(APIENTRY* Disable)(GLenum);
    void(APIENTRY* Enable)(GLenum);
    void(APIENTRY* Color4f)(GLfloat, GLfloat, GLfloat, GLfloat);
    void(APIENTRY* Begin)(GLenum);
    void(APIENTRY* End)();
    void(APIENTRY* TexCoord2f)(GLfloat, GLfloat);
    void(APIENTRY* Vertex2f)(GLfloat, GLfloat);
    void(APIENTRY* ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
    void(APIENTRY* DepthMask)(GLboolean);
    void(APIENTRY* DrawBuffer)(GLenum);
    void(APIENTRY* ReadBuffer)(GLenum);
    GLboolean(APIENTRY* IsEnabled)(GLenum);
    GLenum(APIENTRY* GetError)();
    HGLRC(WINAPI* GetCurrentContext)();
} gl;

int g_loadState;  // 0 not tried, 1 ready, -1 unavailable
bool g_failed;    // creation failed once: stay on the window for this run
HGLRC g_ctx;
int g_w, g_h, g_samples, g_asked;  // g_asked: the sample count asked for (g_samples: what the driver gave)
GLuint g_msFbo, g_msColor, g_msDepth;  // multisampled draw target (0 when samples <= 1)
GLuint g_fbo, g_tex, g_depth;          // single-sample: the draw target without MSAA, the resolve target with it
GLuint g_winFbo, g_winColor;           // a window-sized copy for the engine's own frame grabs
int g_winW, g_winH;
bool g_active, g_resolved;

bool Load(PROC(WINAPI* getProc)(LPCSTR)) {
    if (g_loadState) return g_loadState > 0;
    g_loadState = -1;
    HMODULE m = GetModuleHandleA("opengl32.dll");
#define P(name, sym) gl.name = (decltype(gl.name))GetProcAddress(m, sym)
#define X(name, sym) gl.name = (decltype(gl.name))getProc(sym)
    X(GenFramebuffers, "glGenFramebuffersEXT"); X(DeleteFramebuffers, "glDeleteFramebuffersEXT");
    X(BindFramebuffer, "glBindFramebufferEXT"); X(GenRenderbuffers, "glGenRenderbuffersEXT");
    X(DeleteRenderbuffers, "glDeleteRenderbuffersEXT"); X(BindRenderbuffer, "glBindRenderbufferEXT");
    X(RenderbufferStorage, "glRenderbufferStorageEXT");
    X(RenderbufferStorageMultisample, "glRenderbufferStorageMultisampleEXT");
    X(FramebufferRenderbuffer, "glFramebufferRenderbufferEXT"); X(FramebufferTexture2D, "glFramebufferTexture2DEXT");
    X(CheckFramebufferStatus, "glCheckFramebufferStatusEXT"); X(BlitFramebuffer, "glBlitFramebufferEXT");
    X(ActiveTexture, "glActiveTextureARB");
    P(Viewport, "glViewport"); P(GetIntegerv, "glGetIntegerv"); P(Clear, "glClear"); P(GenTextures, "glGenTextures");
    P(DeleteTextures, "glDeleteTextures"); P(BindTexture, "glBindTexture"); P(TexImage2D, "glTexImage2D");
    P(TexParameteri, "glTexParameteri"); P(PushAttrib, "glPushAttrib"); P(PopAttrib, "glPopAttrib");
    P(MatrixMode, "glMatrixMode"); P(PushMatrix, "glPushMatrix"); P(PopMatrix, "glPopMatrix");
    P(LoadIdentity, "glLoadIdentity"); P(Disable, "glDisable"); P(Enable, "glEnable"); P(Color4f, "glColor4f");
    P(Begin, "glBegin"); P(End, "glEnd"); P(TexCoord2f, "glTexCoord2f"); P(Vertex2f, "glVertex2f");
    P(ColorMask, "glColorMask"); P(DepthMask, "glDepthMask"); P(DrawBuffer, "glDrawBuffer"); P(ReadBuffer, "glReadBuffer");
    P(IsEnabled, "glIsEnabled");
    P(GetError, "glGetError"); P(GetCurrentContext, "wglGetCurrentContext");
#undef P
#undef X
    if (!gl.GenFramebuffers || !gl.BindFramebuffer || !gl.GenRenderbuffers || !gl.RenderbufferStorage ||
        !gl.FramebufferRenderbuffer || !gl.FramebufferTexture2D || !gl.CheckFramebufferStatus || !gl.ReadBuffer ||
        !gl.BlitFramebuffer) {
        Log("eyebuf: EXT_framebuffer_object / _blit not available - eyes stay at the window's size");
        return false;
    }
    if (!gl.RenderbufferStorageMultisample)
        Log("eyebuf: no multisample framebuffers (EXT_framebuffer_multisample) - eyes without anti-aliasing");
    g_loadState = 1;
    return true;
}

void Destroy() {
    if (g_msFbo) gl.DeleteFramebuffers(1, &g_msFbo);
    if (g_fbo) gl.DeleteFramebuffers(1, &g_fbo);
    GLuint rb[] = {g_msColor, g_msDepth, g_depth};
    for (GLuint r : rb)
        if (r) gl.DeleteRenderbuffers(1, &r);
    if (g_tex) gl.DeleteTextures(1, &g_tex);
    if (g_winFbo) gl.DeleteFramebuffers(1, &g_winFbo);
    if (g_winColor) gl.DeleteRenderbuffers(1, &g_winColor);
    g_msFbo = g_msColor = g_msDepth = g_fbo = g_tex = g_depth = g_winFbo = g_winColor = 0;
    g_w = g_h = g_winW = g_winH = 0;
}

GLuint Renderbuffer(GLenum format, int samples, int w, int h) {
    GLuint r = 0;
    gl.GenRenderbuffers(1, &r);
    gl.BindRenderbuffer(GL_RENDERBUFFER_EXT, r);
    if (samples > 1) gl.RenderbufferStorageMultisample(GL_RENDERBUFFER_EXT, samples, format, w, h);
    else gl.RenderbufferStorage(GL_RENDERBUFFER_EXT, format, w, h);
    gl.BindRenderbuffer(GL_RENDERBUFFER_EXT, 0);
    return r;
}

void AttachDepth(GLuint rb) {
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, rb);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, rb);
}

bool Create(int w, int h, int samples) {
    Destroy();
    GLint maxSamples = 0;
    if (gl.RenderbufferStorageMultisample && gl.BlitFramebuffer) gl.GetIntegerv(GL_MAX_SAMPLES_EXT, &maxSamples);
    if (samples > maxSamples) samples = maxSamples;
    if (samples < 2) samples = 0;
    GLint prevTex = 0;
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    // single-sample colour is a texture: the end-of-pass copy into the (possibly multisampled) window is a quad
    gl.GenTextures(1, &g_tex);
    gl.BindTexture(GL_TEXTURE_2D, g_tex);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE_);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE_);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.BindTexture(GL_TEXTURE_2D, prevTex);
    g_depth = Renderbuffer(GL_DEPTH24_STENCIL8_EXT, 0, w, h);
    gl.GenFramebuffers(1, &g_fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, g_fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, g_tex, 0);
    AttachDepth(g_depth);
    GLenum st = gl.CheckFramebufferStatus(GL_FRAMEBUFFER_EXT);
    GLenum stMs = GL_FRAMEBUFFER_COMPLETE_EXT;
    if (st == GL_FRAMEBUFFER_COMPLETE_EXT && samples) {
        g_msColor = Renderbuffer(GL_RGBA8, samples, w, h);
        g_msDepth = Renderbuffer(GL_DEPTH24_STENCIL8_EXT, samples, w, h);
        gl.GenFramebuffers(1, &g_msFbo);
        gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, g_msFbo);
        gl.FramebufferRenderbuffer(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, g_msColor);
        AttachDepth(g_msDepth);
        stMs = gl.CheckFramebufferStatus(GL_FRAMEBUFFER_EXT);
    }
    gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, 0);
    while (gl.GetError() != GL_NO_ERROR) {}
    if (st != GL_FRAMEBUFFER_COMPLETE_EXT) {
        Log("eyebuf: %dx%d framebuffer incomplete (0x%x) - eyes stay at the window's size", w, h, st);
        Destroy();
        return false;
    }
    if (stMs != GL_FRAMEBUFFER_COMPLETE_EXT) {
        Log("eyebuf: %dx samples incomplete (0x%x) - eyes without anti-aliasing", samples, stMs);
        gl.DeleteFramebuffers(1, &g_msFbo);
        gl.DeleteRenderbuffers(1, &g_msColor);
        gl.DeleteRenderbuffers(1, &g_msDepth);
        g_msFbo = g_msColor = g_msDepth = 0;
        samples = 0;
    }
    g_w = w;
    g_h = h;
    g_samples = samples;
    Log("eyebuf: eyes drawn at %dx%d, %dx anti-aliasing", w, h, samples ? samples : 1);
    return true;
}

}  // namespace

bool EyeBuf_Begin(int w, int h, int samples, PROC(WINAPI* getProc)(LPCSTR)) {
    if (g_active) return true;
    if (g_failed || w <= 0 || h <= 0 || !Load(getProc)) return false;
    HGLRC ctx = gl.GetCurrentContext();
    if (ctx != g_ctx) {  // framebuffer objects belong to one context: a new one (reset) starts over
        g_ctx = ctx;
        g_msFbo = g_msColor = g_msDepth = g_fbo = g_tex = g_depth = g_winFbo = g_winColor = 0;
        g_w = g_h = g_winW = g_winH = 0;
    }
    if (w != g_w || h != g_h || samples != g_asked) {
        g_asked = samples;
        if (!Create(w, h, samples)) { g_failed = true; return false; }
    }
    gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, g_msFbo ? g_msFbo : g_fbo);
    gl.DrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
    gl.ReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    g_active = true;
    g_resolved = false;
    gl.Viewport(0, 0, g_w, g_h);
    // the engine cleared the window just before its world camera (colour, depth, stencil): the same for us, with
    // its clear values, masks and scissor exactly as it left them
    gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    return true;
}

bool EyeBuf_Active() { return g_active; }
int EyeBuf_Width() { return g_w; }
int EyeBuf_Height() { return g_h; }

void EyeBuf_Resolve() {
    if (!g_active || g_resolved) return;
    g_resolved = true;
    if (!g_msFbo) return;
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER_EXT, g_msFbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER_EXT, g_fbo);
    GLboolean on = gl.IsEnabled(GL_SCISSOR_TEST);
    if (on) gl.Disable(GL_SCISSOR_TEST);  // blits obey the scissor
    gl.BlitFramebuffer(0, 0, g_w, g_h, 0, 0, g_w, g_h, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                       GL_NEAREST);
    if (on) gl.Enable(GL_SCISSOR_TEST);
    gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, g_fbo);
    gl.DrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
    gl.ReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
}

void EyeBuf_End(int winW, int winH) {
    if (!g_active) return;
    EyeBuf_Resolve();
    g_active = false;
    gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, 0);
    gl.DrawBuffer(GL_BACK);
    gl.ReadBuffer(GL_BACK);
    // the eye, stretched over the window as it always was: a plain textured quad (a blit can't target a
    // multisampled window, which KotOR's anti-aliasing setting gives us)
    gl.PushAttrib(GL_ALL_ATTRIB_BITS);
    gl.MatrixMode(GL_TEXTURE);
    gl.PushMatrix();
    gl.LoadIdentity();
    gl.MatrixMode(GL_PROJECTION);
    gl.PushMatrix();
    gl.LoadIdentity();
    gl.MatrixMode(GL_MODELVIEW);
    gl.PushMatrix();
    gl.LoadIdentity();
    gl.Viewport(0, 0, winW, winH);
    const GLenum off[] = {GL_DEPTH_TEST, GL_CULL_FACE, GL_LIGHTING, GL_ALPHA_TEST, GL_FOG, GL_STENCIL_TEST, GL_BLEND,
                          GL_SCISSOR_TEST, GL_VERTEX_PROGRAM_ARB_, GL_FRAGMENT_PROGRAM_ARB_, GL_REGISTER_COMBINERS_NV_,
                          GL_TEXTURE_SHADER_NV_, GL_TEXTURE_GEN_S, GL_TEXTURE_GEN_T};
    for (GLenum e : off) gl.Disable(e);
    if (gl.ActiveTexture) {
        for (int u = 3; u >= 1; u--) {
            gl.ActiveTexture(GL_TEXTURE0_ARB_ + u);
            gl.Disable(GL_TEXTURE_2D);
        }
        gl.ActiveTexture(GL_TEXTURE0_ARB_);
    }
    gl.Enable(GL_TEXTURE_2D);
    gl.BindTexture(GL_TEXTURE_2D, g_tex);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.DepthMask(GL_FALSE);
    gl.Color4f(1, 1, 1, 1);
    gl.Begin(GL_QUADS);
    gl.TexCoord2f(0, 0); gl.Vertex2f(-1, -1);
    gl.TexCoord2f(1, 0); gl.Vertex2f(1, -1);
    gl.TexCoord2f(1, 1); gl.Vertex2f(1, 1);
    gl.TexCoord2f(0, 1); gl.Vertex2f(-1, 1);
    gl.End();
    gl.MatrixMode(GL_MODELVIEW);
    gl.PopMatrix();
    gl.MatrixMode(GL_PROJECTION);
    gl.PopMatrix();
    gl.MatrixMode(GL_TEXTURE);
    gl.PopMatrix();
    gl.PopAttrib();
    while (gl.GetError() != GL_NO_ERROR) {}  // the NV enables may be unknown on this driver
}

// (only in the context that owns the eye buffer: KotOR's glow passes draw into pbuffers with contexts of their own)
static bool Bound() { return g_active && gl.GetCurrentContext() == g_ctx; }

void EyeBuf_MapViewport(GLint* x, GLint* y, GLsizei* w, GLsizei* h, int winW, int winH) {
    if (!Bound() || winW <= 0 || winH <= 0) return;
    if (*x == 0 && *y == 0 && *w == winW && *h == winH) {
        *w = g_w;
        *h = g_h;
    }
}

bool EyeBuf_BeginWindowRead(GLint x, GLint y, GLsizei w, GLsizei h, int winW, int winH) {
    if (!Bound() || x != 0 || y != 0 || w != winW || h != winH || winW <= 0 || winH <= 0) return false;
    if (winW != g_winW || winH != g_winH) {
        if (g_winFbo) gl.DeleteFramebuffers(1, &g_winFbo);
        if (g_winColor) gl.DeleteRenderbuffers(1, &g_winColor);
        g_winColor = Renderbuffer(GL_RGBA8, 0, winW, winH);
        gl.GenFramebuffers(1, &g_winFbo);
        gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, g_winFbo);
        gl.FramebufferRenderbuffer(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, g_winColor);
        GLenum st = gl.CheckFramebufferStatus(GL_FRAMEBUFFER_EXT);
        gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, g_resolved || !g_msFbo ? g_fbo : g_msFbo);
        g_winW = winW;
        g_winH = winH;
        if (st != GL_FRAMEBUFFER_COMPLETE_EXT) {
            Log("eyebuf: window-sized copy incomplete (0x%x) - glow reads the corner of the eye buffer", st);
            return false;
        }
    }
    GLboolean scissor = gl.IsEnabled(GL_SCISSOR_TEST);
    if (scissor) gl.Disable(GL_SCISSOR_TEST);  // blits obey the scissor
    GLuint drawn = g_resolved || !g_msFbo ? g_fbo : g_msFbo;
    if (drawn == g_msFbo) {  // a multisampled buffer can only be resolved 1:1 - into the single-sample one first
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER_EXT, g_msFbo);
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER_EXT, g_fbo);
        gl.BlitFramebuffer(0, 0, g_w, g_h, 0, 0, g_w, g_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER_EXT, g_fbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER_EXT, g_winFbo);
    gl.BlitFramebuffer(0, 0, g_w, g_h, 0, 0, winW, winH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER_EXT, drawn);  // the engine keeps drawing where it was
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER_EXT, g_winFbo);
    gl.ReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    if (scissor) gl.Enable(GL_SCISSOR_TEST);
    return true;
}

void EyeBuf_EndWindowRead() {
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER_EXT, g_resolved || !g_msFbo ? g_fbo : g_msFbo);
    gl.ReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
}

void EyeBuf_UnmapViewport(GLint* v, int winW, int winH) {
    if (!Bound() || winW <= 0 || winH <= 0) return;
    if (v[0] == 0 && v[1] == 0 && v[2] == g_w && v[3] == g_h) {
        v[2] = winW;
        v[3] = winH;
    }
}

GLenum EyeBuf_MapDrawBuffer(GLenum b) {
    if (Bound() && (b == GL_BACK || b == GL_FRONT || b == GL_BACK_LEFT || b == GL_FRONT_LEFT || b == GL_FRONT_AND_BACK))
        return GL_COLOR_ATTACHMENT0_EXT;
    return b;
}
