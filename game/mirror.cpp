// Desktop mirror in the game's own window (see mirror.h).
//
// In the world the window's back buffer holds only the GUI at swap time (the world image is captured for the
// headset and cleared away before the GUI pass). On left-eye frames we copy that eye image into a texture just
// before the clear (one GPU copy), and every world frame we draw it under the GUI with a screen blend
// (ONE_MINUS_DST_COLOR, ONE): where the GUI is black the eye shows, where the GUI is drawn it stays on top. The
// right-eye frames reuse the kept left eye, so the monitor shows a steady mono view (as in d2r-pcvr's
// desktop_one_eye). Menus and movies are drawn by the game itself and need nothing.
#include "mirror.h"

#include <GL/gl.h>

extern void Log(const char* fmt, ...);

#ifndef GL_TEXTURE0_ARB
#define GL_TEXTURE0_ARB 0x84C0
#endif
#define GL_VERTEX_PROGRAM_ARB_ 0x8620
#define GL_FRAGMENT_PROGRAM_ARB_ 0x8804
#define GL_REGISTER_COMBINERS_NV_ 0x8522
#define GL_TEXTURE_SHADER_NV_ 0x86DE
#define GL_CLAMP_TO_EDGE_ 0x812F

namespace {

struct Gl {
    void(APIENTRY* GenTextures)(GLsizei, GLuint*);
    void(APIENTRY* BindTexture)(GLenum, GLuint);
    void(APIENTRY* TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
    void(APIENTRY* CopyTexSubImage2D)(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
    void(APIENTRY* TexParameteri)(GLenum, GLenum, GLint);
    void(APIENTRY* PushAttrib)(GLbitfield);
    void(APIENTRY* PopAttrib)();
    void(APIENTRY* MatrixMode)(GLenum);
    void(APIENTRY* PushMatrix)();
    void(APIENTRY* PopMatrix)();
    void(APIENTRY* LoadIdentity)();
    void(APIENTRY* Disable)(GLenum);
    void(APIENTRY* Enable)(GLenum);
    void(APIENTRY* BlendFunc)(GLenum, GLenum);
    void(APIENTRY* Color4f)(GLfloat, GLfloat, GLfloat, GLfloat);
    void(APIENTRY* Begin)(GLenum);
    void(APIENTRY* End)();
    void(APIENTRY* TexCoord2f)(GLfloat, GLfloat);
    void(APIENTRY* Vertex2f)(GLfloat, GLfloat);
    void(APIENTRY* Viewport)(GLint, GLint, GLsizei, GLsizei);
    void(APIENTRY* ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
    void(APIENTRY* DepthMask)(GLboolean);
    GLenum(APIENTRY* GetError)();
    void(APIENTRY* ActiveTexture)(GLenum);  // ARB, may be missing
    HGLRC(WINAPI* GetCurrentContext)();
} gl;

bool g_loaded;
GLuint g_tex;
int g_w, g_h;
HGLRC g_ctx;
DWORD g_keptAt;

bool Load(PROC(WINAPI* getProc)(LPCSTR)) {
    if (g_loaded) return gl.GenTextures != nullptr;
    g_loaded = true;
    HMODULE m = GetModuleHandleA("opengl32.dll");
#define P(name, sym) gl.name = (decltype(gl.name))GetProcAddress(m, sym)
    P(GenTextures, "glGenTextures"); P(BindTexture, "glBindTexture"); P(TexImage2D, "glTexImage2D");
    P(CopyTexSubImage2D, "glCopyTexSubImage2D"); P(TexParameteri, "glTexParameteri"); P(PushAttrib, "glPushAttrib");
    P(PopAttrib, "glPopAttrib"); P(MatrixMode, "glMatrixMode"); P(PushMatrix, "glPushMatrix"); P(PopMatrix, "glPopMatrix");
    P(LoadIdentity, "glLoadIdentity"); P(Disable, "glDisable"); P(Enable, "glEnable"); P(BlendFunc, "glBlendFunc");
    P(Color4f, "glColor4f"); P(Begin, "glBegin"); P(End, "glEnd"); P(TexCoord2f, "glTexCoord2f"); P(Vertex2f, "glVertex2f");
    P(Viewport, "glViewport"); P(ColorMask, "glColorMask"); P(DepthMask, "glDepthMask"); P(GetError, "glGetError");
    P(GetCurrentContext, "wglGetCurrentContext");
#undef P
    gl.ActiveTexture = (decltype(gl.ActiveTexture))getProc("glActiveTextureARB");
    bool ok = gl.GenTextures && gl.CopyTexSubImage2D && gl.Begin && gl.PushAttrib && gl.GetCurrentContext;
    if (!ok) { gl.GenTextures = nullptr; Log("mirror: GL entry points missing, desktop mirror off"); }
    return ok;
}

}  // namespace

void Mirror_KeepEye(int w, int h, PROC(WINAPI* getProc)(LPCSTR)) {
    if (!Load(getProc)) return;
    HGLRC ctx = gl.GetCurrentContext();
    if (!g_tex || ctx != g_ctx || w != g_w || h != g_h) {
        gl.GenTextures(1, &g_tex);  // (a new context or size: the old name is simply abandoned)
        gl.BindTexture(GL_TEXTURE_2D, g_tex);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE_);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE_);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        g_ctx = ctx;
        g_w = w;
        g_h = h;
        Log("mirror: eye texture %dx%d", w, h);
    }
    gl.PushAttrib(GL_TEXTURE_BIT);
    if (gl.ActiveTexture) gl.ActiveTexture(GL_TEXTURE0_ARB);
    gl.BindTexture(GL_TEXTURE_2D, g_tex);
    gl.CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    gl.PopAttrib();
    g_keptAt = GetTickCount();
}

void Mirror_Draw() {
    if (!g_tex || !gl.GenTextures || gl.GetCurrentContext() != g_ctx) return;
    if (GetTickCount() - g_keptAt > 500) return;  // no fresh eye (pick frames, loading): leave the frame alone
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
    gl.Viewport(0, 0, g_w, g_h);
    const GLenum off[] = {GL_DEPTH_TEST, GL_CULL_FACE, GL_LIGHTING, GL_ALPHA_TEST, GL_FOG, GL_STENCIL_TEST,
                          GL_SCISSOR_TEST, GL_VERTEX_PROGRAM_ARB_, GL_FRAGMENT_PROGRAM_ARB_, GL_REGISTER_COMBINERS_NV_,
                          GL_TEXTURE_SHADER_NV_, GL_TEXTURE_GEN_S, GL_TEXTURE_GEN_T};
    for (GLenum e : off) gl.Disable(e);
    if (gl.ActiveTexture) {
        for (int u = 3; u >= 1; u--) {
            gl.ActiveTexture(GL_TEXTURE0_ARB + u);
            gl.Disable(GL_TEXTURE_2D);
        }
        gl.ActiveTexture(GL_TEXTURE0_ARB);
    }
    gl.Enable(GL_TEXTURE_2D);
    gl.BindTexture(GL_TEXTURE_2D, g_tex);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    gl.DepthMask(GL_FALSE);
    gl.Enable(GL_BLEND);
    gl.BlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE);  // screen: the GUI stays on top, the eye fills the black
    gl.Color4f(1, 1, 1, 1);
    gl.Begin(GL_QUADS);
    // the headset's field of view is far wider than a monitor's: show its central 70% so it reads naturally
    const float lo = 0.15f, hi = 0.85f;
    gl.TexCoord2f(lo, lo); gl.Vertex2f(-1, -1);
    gl.TexCoord2f(hi, lo); gl.Vertex2f(1, -1);
    gl.TexCoord2f(hi, hi); gl.Vertex2f(1, 1);
    gl.TexCoord2f(lo, hi); gl.Vertex2f(-1, 1);
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
