// The Ebon Hawk's turret minigame (m12ab) in VR.
//
// The game aims the turret with the mouse (its .are MiniGame Mouse AxisX / AxisY, loaded in 0x6723D0) and renders
// from the gunner camera, which turns with the aim. Riding that camera turned your whole body with every aim, and the
// stick (W/A/S/D) plus the hand pointer's cursor fought over it (2026-11-13 playtest). Now:
//   - the view stays in the gunner's seat (enginecam: the seat's heading is fixed, your head looks around freely);
//   - the right hand aims: each frame the turret's aim (the game camera's forward) is compared with where the hand
//     points, and mouse movement closes the gap (proportional, with the counts per radian measured at the start);
//   - the right trigger (or A) fires, B / menu pause; the stick no longer aims;
//   - an orange reticle shows where the guns point, a faint blue ring and short laser where the hand points.
// The response is measured by a short probe once the turret starts (a few degrees each way on each axis): it finds
// the sign and scale of the game's mouse sensitivity / FlipAxis settings, and whether the game reads the DirectInput
// mouse or the cursor. If neither moves the turret the stick takes over again, as before.
#include "turret.h"

#include <dinput.h>
#include <GL/gl.h>
#include <cmath>
#include <cstring>

#include "../common/kvr_shared.h"
#include "enginecam.h"
#include "vr.h"

extern void Log(const char* fmt, ...);

namespace {

const float kPi = 3.14159265f;
const int kProbeCounts = 12;     // mouse counts per frame while probing
const int kProbeFrames = 10;     // frames of movement per probe step
const int kSettleFrames = 10;    // frames after each step whose motion still belongs to it (latency, inertia)
const float kMinResponse = 0.005f;  // rad: less over a probe step pair = this channel doesn't move the turret
const float kGain = 0.3f;        // fraction of the aim error closed per frame
const float kDeadband = 0.0025f; // rad (~0.15 deg): hand tremor below this isn't chased
const LONG kMaxCounts = 150;  // ~1.6 deg a frame at the rig's measured scale
const float kReticleDist = 30.0f;  // m

float WrapPi(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}
float Yaw(const float d[3]) { return atan2f(d[1], d[0]); }
float Pitch(const float d[3]) { return asinf(d[2] < -1 ? -1 : d[2] > 1 ? 1 : d[2]); }
void Normalize(float v[3]) {
    float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) + 1e-9f;
    v[0] /= n, v[1] /= n, v[2] /= n;
}

// session
unsigned g_session;
DWORD g_startTick;
int g_channel = TURRET_DI_MOUSE;
int g_tries;
// probe: 0 waiting for the turret to take input, 1..8 = (x+, settle, x-, settle, y+, settle, y-, settle), 9 done
int g_stage, g_stageFrames;
DWORD g_stageTick;
float g_sum[4];
float g_gx, g_gy;  // radians of yaw / pitch per mouse count
// observation
unsigned g_lastFrame;
bool g_haveLast;
float g_lastYaw, g_lastPitch;
float g_fracX, g_fracY;
// hand (smoothed) and what the reticles draw
bool g_haveHand;
float g_hand[3];
DWORD g_handTick;
bool g_draw;
DWORD g_drawTick;
float g_seatPos[3], g_gun[3], g_handPos[3];

void Reset(unsigned session) {
    g_session = session;
    g_startTick = GetTickCount();
    g_channel = TURRET_DI_MOUSE;
    g_tries = 0;
    g_stage = 0;
    g_stageFrames = 0;
    g_stageTick = g_startTick;
    memset(g_sum, 0, sizeof g_sum);
    g_gx = g_gy = 0;
    g_haveLast = false;
    g_fracX = g_fracY = 0;
    g_haveHand = false;
    Log("turret: started - right hand aims, trigger / A fires, B pauses; measuring the turret's response");
}

// One probe step's movement (stages 1, 3, 5, 7 move; 2, 4, 6, 8 settle).
void ProbeMove(LONG* dx, LONG* dy) {
    int s = g_stage;
    if (s == 1) *dx = kProbeCounts;
    if (s == 3) *dx = -kProbeCounts;
    if (s == 5) *dy = kProbeCounts;
    if (s == 7) *dy = -kProbeCounts;
}

void ProbeFinish() {
    float span = 2.0f * kProbeFrames * kProbeCounts;
    float rx = g_sum[0] - g_sum[1], ry = g_sum[2] - g_sum[3];
    bool okX = fabsf(rx) > kMinResponse, okY = fabsf(ry) > kMinResponse;
    Log("turret: probe on %s: x %.2f / %.2f deg, y %.2f / %.2f deg", g_channel == TURRET_DI_MOUSE ? "the mouse" : "the cursor",
        g_sum[0] * 57.29578f, g_sum[1] * 57.29578f, g_sum[2] * 57.29578f, g_sum[3] * 57.29578f);
    if (!okX && !okY) {
        g_tries++;
        if (g_tries >= 4) {  // two tries per channel
            g_channel = TURRET_STICK;
            g_stage = 9;
            Log("turret: the turret doesn't answer the mouse or the cursor - the stick aims (as before)");
            return;
        }
        g_channel = g_tries % 2 == 1 ? TURRET_CURSOR : TURRET_DI_MOUSE;
        g_stage = 0;
        g_stageTick = GetTickCount();
        memset(g_sum, 0, sizeof g_sum);
        Log("turret: no response - trying %s", g_channel == TURRET_CURSOR ? "the cursor" : "the mouse again");
        return;
    }
    g_gx = okX ? rx / span : 0;
    g_gy = okY ? ry / span : 0;
    // an axis that didn't move (held at a limit during the probe): the other axis' scale, the usual sign
    if (!okX) g_gx = -fabsf(g_gy);
    if (!okY) g_gy = -fabsf(g_gx);
    g_stage = 9;
    Log("turret: aiming with the hand: %.4f deg yaw per count, %.4f deg pitch per count%s", g_gx * 57.29578f,
        g_gy * 57.29578f, okX && okY ? "" : " (one axis guessed)");
}

}  // namespace

int Turret_Input(const KvrInput& in, BYTE keys[256], LONG* dx, LONG* dy) {
    *dx = *dy = 0;
    if (in.trigR > 0.6f || (in.buttons & KVR_BTN_A)) keys[DIK_SPACE] = 0x80;
    if (in.buttons & (KVR_BTN_B | KVR_BTN_MENU)) keys[DIK_ESCAPE] = 0x80;
    static bool fireWas;
    bool fire = keys[DIK_SPACE] != 0;
    if (fire && !fireWas) VR_Haptic(1, 0.35f, 0.04f, 120);
    fireWas = fire;

    float seat[3], R[9], gun[3];
    unsigned frame, session;
    if (!EngineCam_Turret(seat, R, gun, &frame, &session)) {
        g_draw = false;
        return g_channel;
    }
    if (session != g_session) Reset(session);
    if (g_channel == TURRET_STICK) {  // fallback: the stick as before
        if (in.lx < -0.4f) keys[DIK_A] = 0x80;
        if (in.lx > 0.4f) keys[DIK_D] = 0x80;
        if (in.ly > 0.4f) keys[DIK_W] = 0x80;
        if (in.ly < -0.4f) keys[DIK_S] = 0x80;
    }

    // where the right hand points, in the world (tracking -> world through the seat)
    DWORD now = GetTickCount();
    if (in.active) {
        const float* q = in.aim.quat;  // x, y, z, w
        float x = q[0], y = q[1], z = q[2], w = q[3];
        float fT[3] = {-2 * (x * z + y * w), -2 * (y * z - x * w), -(1 - 2 * (x * x + y * y))};  // -Z
        float fW[3], pW[3];
        for (int r = 0; r < 3; r++) {
            fW[r] = R[r * 3 + 0] * fT[0] + R[r * 3 + 1] * fT[1] + R[r * 3 + 2] * fT[2];
            pW[r] = seat[r] + R[r * 3 + 0] * in.aim.pos[0] + R[r * 3 + 1] * in.aim.pos[1] + R[r * 3 + 2] * in.aim.pos[2];
        }
        Normalize(fW);
        float dt = g_haveHand ? (now - g_handTick) / 1000.0f : 1.0f;
        float a = 1.0f - expf(-dt / 0.03f);  // light smoothing: tremor, not lag
        for (int i = 0; i < 3; i++) g_hand[i] = g_haveHand ? g_hand[i] + (fW[i] - g_hand[i]) * a : fW[i];
        Normalize(g_hand);
        g_haveHand = true;
        g_handTick = now;
        memcpy(g_handPos, pW, sizeof g_handPos);
    }
    memcpy(g_seatPos, seat, sizeof g_seatPos);
    memcpy(g_gun, gun, sizeof g_gun);
    g_draw = g_haveHand;
    g_drawTick = now;

    if (frame == g_lastFrame) return g_channel;  // no new view of the turret yet
    g_lastFrame = frame;
    float gy = Yaw(gun), gp = Pitch(gun);
    float dYaw = g_haveLast ? WrapPi(gy - g_lastYaw) : 0, dPitch = g_haveLast ? gp - g_lastPitch : 0;
    g_lastYaw = gy, g_lastPitch = gp;
    g_haveLast = true;
    if (g_channel == TURRET_STICK) return g_channel;

    if (g_stage < 9) {
        if (g_stage == 0) {  // let the turret settle in (its intro ignores input) before probing
            if (now - g_stageTick < (g_tries ? 400u : 1500u)) return g_channel;
            g_stage = 1;
            g_stageFrames = 0;
        }
        int acc = (g_stage - 1) / 2;  // 0 x+, 1 x-, 2 y+, 3 y-
        g_sum[acc] += acc < 2 ? dYaw : dPitch;
        ProbeMove(dx, dy);
        if (++g_stageFrames >= ((g_stage & 1) ? kProbeFrames : kSettleFrames)) {
            g_stage++;
            g_stageFrames = 0;
            if (g_stage == 9) ProbeFinish();
        }
        return g_channel;
    }

    // aim: close a fraction of the gap between the guns and the hand every frame
    if (!g_haveHand) return g_channel;
    float eYaw = WrapPi(Yaw(g_hand) - gy), ePitch = Pitch(g_hand) - gp;
    float ix = 0, iy = 0;
    if (fabsf(eYaw) > kDeadband && g_gx != 0) ix = kGain * eYaw / g_gx + g_fracX;
    if (fabsf(ePitch) > kDeadband && g_gy != 0) iy = kGain * ePitch / g_gy + g_fracY;
    LONG cx = lroundf(ix), cy = lroundf(iy);
    g_fracX = ix - cx, g_fracY = iy - cy;
    if (cx > kMaxCounts || cx < -kMaxCounts) cx = cx > 0 ? kMaxCounts : -kMaxCounts, g_fracX = 0;
    if (cy > kMaxCounts || cy < -kMaxCounts) cy = cy > 0 ? kMaxCounts : -kMaxCounts, g_fracY = 0;
    *dx = cx;
    *dy = cy;
    static DWORD lastLog;
    if (now - lastLog > 2000) {
        lastLog = now;
        Log("turret: hand %.1f / %.1f deg, guns %.1f / %.1f deg, off by %.2f / %.2f deg -> %ld %ld counts (%s)",
            Yaw(g_hand) * 57.29578f, Pitch(g_hand) * 57.29578f, gy * 57.29578f, gp * 57.29578f, eYaw * 57.29578f,
            ePitch * 57.29578f, cx, cy, g_channel == TURRET_DI_MOUSE ? "mouse" : "cursor");
    }
    return g_channel;
}

// ---------------------------------------------------------------- reticles

namespace {

typedef void(APIENTRY* glBegin_t)(GLenum);
typedef void(APIENTRY* glEnd_t)();
typedef void(APIENTRY* glVertex3f_t)(GLfloat, GLfloat, GLfloat);
typedef void(APIENTRY* glColor4f_t)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void(APIENTRY* glLineWidth_t)(GLfloat);
glBegin_t gB;
glEnd_t gE;
glVertex3f_t gV;
glColor4f_t gC;
glLineWidth_t gLW;
bool Gl() {
    if (gB) return true;
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) return false;
    gB = (glBegin_t)GetProcAddress(gl, "glBegin");
    gE = (glEnd_t)GetProcAddress(gl, "glEnd");
    gV = (glVertex3f_t)GetProcAddress(gl, "glVertex3f");
    gC = (glColor4f_t)GetProcAddress(gl, "glColor4f");
    gLW = (glLineWidth_t)GetProcAddress(gl, "glLineWidth");
    return gB && gE && gV && gC && gLW;
}
void Vtx(const float wv[16], const float w[3]) {
    float e[3];
    for (int r = 0; r < 3; r++) e[r] = wv[0 * 4 + r] * w[0] + wv[1 * 4 + r] * w[1] + wv[2 * 4 + r] * w[2] + wv[12 + r];
    gV(e[0], e[1], e[2]);
}
// a ring of radius r around c, facing along d (plus 4 ticks when ticks)
void Ring(const float wv[16], const float c[3], const float d[3], float r, bool ticks) {
    float up[3] = {0, 0, 1};
    float u[3] = {d[1] * up[2] - d[2] * up[1], d[2] * up[0] - d[0] * up[2], d[0] * up[1] - d[1] * up[0]};
    Normalize(u);
    float v[3] = {u[1] * d[2] - u[2] * d[1], u[2] * d[0] - u[0] * d[2], u[0] * d[1] - u[1] * d[0]};
    gB(GL_LINE_LOOP);
    for (int k = 0; k < 32; k++) {
        float an = k * (2 * kPi / 32), p[3];
        for (int i = 0; i < 3; i++) p[i] = c[i] + (u[i] * cosf(an) + v[i] * sinf(an)) * r;
        Vtx(wv, p);
    }
    gE();
    if (!ticks) return;
    gB(GL_LINES);
    for (int k = 0; k < 4; k++) {
        float an = k * (kPi / 2), a[3], b[3];
        for (int i = 0; i < 3; i++) {
            float o = u[i] * cosf(an) + v[i] * sinf(an);
            a[i] = c[i] + o * r * 1.35f;
            b[i] = c[i] + o * r * 1.9f;
        }
        Vtx(wv, a);
        Vtx(wv, b);
    }
    gE();
}

}  // namespace

bool Turret_Drawing() { return g_draw && GetTickCount() - g_drawTick < 250; }

void Turret_Draw(const float wv[16]) {
    if (!Turret_Drawing() || !Gl()) return;
    float gc[3], hc[3];
    for (int i = 0; i < 3; i++) gc[i] = g_seatPos[i] + g_gun[i] * kReticleDist, hc[i] = g_seatPos[i] + g_hand[i] * kReticleDist;
    // where the guns point: orange, ~1 deg
    gLW(3.0f);
    gC(1.0f, 0.55f, 0.15f, 0.9f);
    Ring(wv, gc, g_gun, kReticleDist * 0.0175f, true);
    // where the hand points (the guns catch up to it): a faint blue ring, only while they differ
    float cosA = g_gun[0] * g_hand[0] + g_gun[1] * g_hand[1] + g_gun[2] * g_hand[2];
    if (cosA < 0.99985f) {  // > 1 deg
        gLW(2.0f);
        gC(0.35f, 0.78f, 1.0f, 0.45f);
        Ring(wv, hc, g_hand, kReticleDist * 0.009f, false);
    }
    // a short laser from the hand
    float tip[3];
    for (int i = 0; i < 3; i++) tip[i] = g_handPos[i] + g_hand[i] * 1.5f;
    gLW(2.0f);
    gB(GL_LINES);
    gC(0.35f, 0.78f, 1.0f, 0.6f);
    Vtx(wv, g_handPos);
    gC(0.35f, 0.78f, 1.0f, 0.0f);
    Vtx(wv, tip);
    gE();
    gLW(1.0f);
}
