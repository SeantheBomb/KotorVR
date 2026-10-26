// In-game VR layer: per-eye camera substitution (alternate-eye rendering) and frame export.
#pragma once
#include <windows.h>
#include <GL/gl.h>

struct VrGl {
    // real (unhooked) entry points, filled by the hook layer
    void(APIENTRY* MultMatrixf)(const GLfloat*);
    void(APIENTRY* Translatef)(GLfloat, GLfloat, GLfloat);
    void(APIENTRY* Frustum)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
    PROC(WINAPI* GetProcAddress)(LPCSTR);
};

void VR_Init(const VrGl& gl, void (*log)(const char*, ...));
// return true when the call was fully handled (caller must not forward it)
bool VR_OnPerspective(GLdouble fovy, GLdouble aspect, GLdouble zn, GLdouble zf);
bool VR_OnMultMatrixf(GLenum mode, const GLfloat* m);
bool VR_OnTranslatef(GLenum mode, GLfloat x, GLfloat y, GLfloat z);
bool VR_OnEnvParam(GLenum target, GLuint index, GLfloat* xyzw);  // may rewrite xyzw
void VR_OnOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t);
void VR_OnSwap(HDC dc);
bool VR_IsOurTexture(GLuint name);
// True for a node translation (world space) inside the player's own body column around the first-person camera.
bool VR_IsOwnBody(const float t[3], int depth);  // depth: modelview stack depth of the multiply
// Per draw call during the world pass: true when the draw belongs to the player's own body (skip it).
bool VR_ShouldSkipDraw();
bool VR_ShouldSkipElements(GLsizei count, GLenum type, const void* indices);
void VR_OnVertexPointer(GLint size, GLenum type, GLsizei stride, const void* ptr);
// diagnostics: request a one-frame trace (true once), and the current draw origin in the camera frame
bool VR_WantTrace();
void VR_DrawOrigin(float out[3]);
// The loader shares freshly created pbuffer contexts with the main one; the NVIDIA driver faults if the
// main context holds interop-registered objects at that point. Tear the export down first.
void VR_BeforeShareLists();
bool VR_InCapture();

struct KvrShared;
struct KvrPose;
bool VR_IsActive();
// Head yaw relative to the game camera (radians, + = turned left); 0 when VR is inactive.
float VR_HeadYaw();
// World picks along the hand: for `seconds` the engine camera looks along the aim pose (so the screen center is
// exactly what the hand points at) and those frames are not sent to the headset.
void VR_BeginAimPick(const KvrPose& aim, double seconds);
bool VR_AimPick(KvrPose* out);
// Line-of-sight probe for cutscene shots: this frame is rendered from the player's eye looking straight at a point
// `distance` away; after the world pass the depth at the view center says whether anything is in between. The frame
// is not shown. Result next frame: 1 = clear, 0 = blocked, -1 = none pending.
void VR_RequestLosProbe(float distance);
int VR_LosProbeResult();
// Ask the host for one controller rumble pulse (hand 0 left, 1 right); amplitude 0..1, seconds, Hz (0 = default).
void VR_Haptic(int hand, float amplitude, float duration, float frequency);
// Head (eye 0) position in the recenter-origin frame.
void VR_HeadPos(float out[3]);
bool VR_ChooseEye(KvrPose* out);
void VR_EngineCamApplied(const float basePos[3]);
void VR_SetCinematic(bool on);
const KvrShared* VR_Shared();
void VR_ClientSize(int* w, int* h);
// Project a controller aim ray (relative to the recenter origin == game camera frame) into the game's own
// camera to get the client-space cursor position the game would pick along that ray.
bool VR_WorldPointerToClient(const float aimPos[3], const float aimQuat[4], float* x, float* y);
