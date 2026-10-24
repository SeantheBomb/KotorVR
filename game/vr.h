// In-game VR layer: per-eye camera substitution (alternate-eye rendering) and frame export.
#pragma once
#include <windows.h>
#include <GL/gl.h>

struct VrGl {
    // real (unhooked) entry points, filled by the hook layer
    void(APIENTRY* MultMatrixf)(const GLfloat*);
    void(APIENTRY* Frustum)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
    PROC(WINAPI* GetProcAddress)(LPCSTR);
};

void VR_Init(const VrGl& gl, void (*log)(const char*, ...));
// return true when the call was fully handled (caller must not forward it)
bool VR_OnPerspective(GLdouble fovy, GLdouble aspect, GLdouble zn, GLdouble zf);
bool VR_OnMultMatrixf(GLenum mode, const GLfloat* m);
void VR_OnTranslatef(GLenum mode, GLfloat x, GLfloat y, GLfloat z);
bool VR_OnEnvParam(GLenum target, GLuint index, GLfloat* xyzw);  // may rewrite xyzw
void VR_OnOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t);
void VR_OnSwap(HDC dc);
bool VR_IsOurTexture(GLuint name);
// The loader shares freshly created pbuffer contexts with the main one; the NVIDIA driver faults if the
// main context holds interop-registered objects at that point. Tear the export down first.
void VR_BeforeShareLists();
bool VR_InCapture();
