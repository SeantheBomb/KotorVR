// The eye buffer: KotOR's world pass drawn into an offscreen framebuffer at the headset's own per-eye size instead of
// the game window (whose size KotOR, the monitor's display modes and Windows' desktop clamp all limit). See eyebuf.cpp.
#pragma once
#include <windows.h>
#include <GL/gl.h>

// Bind the eye buffer for this frame's world pass (created / resized as needed, cleared as the engine's own clear
// would have been). False: not available here - the world pass stays in the window as before.
bool EyeBuf_Begin(int w, int h, int samples, PROC(WINAPI* getProc)(LPCSTR));
bool EyeBuf_Active();
int EyeBuf_Width();
int EyeBuf_Height();
// Multisampled: resolve colour and depth into the single-sample buffer, which is then bound for reading and drawing
// (readbacks, the depth probe). Idempotent within a pass.
void EyeBuf_Resolve();
// End of the world pass: a scaled copy goes into the window's back buffer (the monitor mirror and everything the
// engine does after the world pass see what they always did), then the window is bound again at its own viewport.
void EyeBuf_End(int winW, int winH);
// Hook helpers while the eye buffer is bound: the engine's full-window viewport covers the whole eye buffer, and
// its back-buffer draw targets mean the eye buffer's colour attachment.
void EyeBuf_MapViewport(GLint* x, GLint* y, GLsizei* w, GLsizei* h, int winW, int winH);
GLenum EyeBuf_MapDrawBuffer(GLenum b);
// ... and a viewport read back while it is bound reads as the window's: KotOR takes its next camera's aspect from it
// (with the eye buffer's every other frame failed the 4:3 world-camera test - half the frames in the headset)
void EyeBuf_UnmapViewport(GLint* v, int winW, int winH);
// The engine copying its frame out (glow grabs the whole window into a texture after the world pass, traced
// 2026-11-10): for a full-window copy while the eye buffer is bound, read from a window-sized scaled copy of it, so
// the glow keeps the size and alignment it always had. True: the read binding was switched - call
// EyeBuf_EndWindowRead after the copy.
bool EyeBuf_BeginWindowRead(GLint x, GLint y, GLsizei w, GLsizei h, int winW, int winH);
void EyeBuf_EndWindowRead();
