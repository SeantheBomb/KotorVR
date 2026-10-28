// Engine camera driving (x86, inside swkotor.exe).
//
// 0x45C540 (virtual, thiscall, no stack args) is the scene camera's per-frame setup: it loads the GL projection
// and modelview from the camera object and builds the culling frustum from the same pose:
//   +0x7C/0x80/0x84  position (world, Z-up)
//   +0x88..0x94      orientation quaternion, w first (w, x, y, z)
//   +0x1D0           vertical FOV, degrees
// We detour it: on entry the camera pose is swapped for the current eye pose (head-tracked, and pinned to the
// player's head during dialogue), so the engine renders AND culls from where the player is looking; on exit the
// original pose is restored so game logic never sees the change.
//
// The quaternion/frame convention is verified at runtime before anything is written: for 30 gameplay frames
// we only compare the view matrix the engine hands to GL against the one implied by the quaternion.

#include "enginecam.h"

#include <windows.h>
#include <cmath>
#include <cstring>

#include "../common/kvr_shared.h"
#include "features.h"
#include "anim.h"
#include "vr.h"
#include "weapons.h"

extern void Log(const char* fmt, ...);

namespace {

const DWORD kCamSetupVA = 0x45C540;
const BYTE kCamSetupOrig[9] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x68};
const float kGameplayFov = 170.0f;   // camerastyle.2da viewangle in Override

BYTE* g_tramp;

// verification
bool g_verified, g_gaveUp;
int g_matchFrames;
bool g_pending;
float g_obsQ[4], g_obsPos[3];

// driving
bool g_applied;
BYTE* g_cam;
float g_origPos[3], g_origQ[4];
bool g_havePlay;
float g_playPos[3], g_playQ[4];
bool g_haveEyeOffset;
float g_eyeOffset[3];   // gameplay camera position minus the controlled creature's position (eye height, etc.)
float g_followPos[3];   // cutscene anchor: the creature's live position + that offset
// owned heading (see EngineCam_Turn)
bool g_viewYawValid;
float g_viewYaw, g_gameYaw, g_lastPlayPos[3];
volatile float g_pendingTurn;

float WrapPi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

void QuatFromRows(const float m[9], float q[4]);

// camera (GL camera space: x right, y up, -z forward) -> world (KotOR: z up), level, facing heading psi (CCW from +X)
void QuatFromYaw(float psi, float q[4]) {
    float s = sinf(psi), c = cosf(psi);
    float m[9] = {s, 0, -c, -c, 0, -s, 0, 1, 0};  // rows; columns = camera x, y, z in world
    QuatFromRows(m, q);
}

// camera looking along world direction f (any pitch), up kept toward +Z
void QuatLookAlong(const float f[3], float q[4]) {
    float n = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]) + 1e-6f;
    float F[3] = {f[0] / n, f[1] / n, f[2] / n};
    float h = sqrtf(F[0] * F[0] + F[1] * F[1]) + 1e-6f;
    float Rt[3] = {F[1] / h, -F[0] / h, 0};  // right = forward x up (level)
    float Z[3] = {-F[0], -F[1], -F[2]};
    float U[3] = {Z[1] * Rt[2] - Z[2] * Rt[1], Z[2] * Rt[0] - Z[0] * Rt[2], Z[0] * Rt[1] - Z[1] * Rt[0]};  // y = z x x
    float m[9] = {Rt[0], U[0], Z[0], Rt[1], U[1], Z[1], Rt[2], U[2], Z[2]};
    QuatFromRows(m, q);
}

void QuatFromRows(const float m[9], float q[4]) {
    float tr = m[0] + m[4] + m[8];
    if (tr > 0) {
        float k = sqrtf(tr + 1.0f) * 2;
        q[0] = 0.25f * k; q[1] = (m[7] - m[5]) / k; q[2] = (m[2] - m[6]) / k; q[3] = (m[3] - m[1]) / k;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        float k = sqrtf(1.0f + m[0] - m[4] - m[8]) * 2;
        q[0] = (m[7] - m[5]) / k; q[1] = 0.25f * k; q[2] = (m[1] + m[3]) / k; q[3] = (m[2] + m[6]) / k;
    } else if (m[4] > m[8]) {
        float k = sqrtf(1.0f + m[4] - m[0] - m[8]) * 2;
        q[0] = (m[2] - m[6]) / k; q[1] = (m[1] + m[3]) / k; q[2] = 0.25f * k; q[3] = (m[5] + m[7]) / k;
    } else {
        float k = sqrtf(1.0f + m[8] - m[0] - m[4]) * 2;
        q[0] = (m[3] - m[1]) / k; q[1] = (m[2] + m[6]) / k; q[2] = (m[5] + m[7]) / k; q[3] = 0.25f * k;
    }
}

// Cutscene shots (2026-10-27 rules, after "cut away less" overshot and kept even Carth's communicator call first person):
//   1. a reverse shot - the shot camera faces back at you (> 110 deg from your facing) or looks straight at you from
//      close by - never cuts: those are the over-the-shoulder / "camera looking at the player" shots;
//   2. otherwise the shot's subject (two hidden probe frames: the shot camera's view centre, then the eye toward it):
//      first person if you can see it from where you stand, the game's shot if it is in another room / around a
//      corner (comm calls, scenes elsewhere).
float g_shotPos[3], g_shotQ[4];
float g_subject[3];  // the point the current shot looks at
// 0 none, 1 first person, 2 cut, 3 subject probe this frame, 4 waiting for it, 5 eye probe this frame, 6 waiting
int g_shotMode;

// aim picks (VR_BeginAimPick): the camera looks along the hand and is left that way for the game's own mouse picking
bool g_pickApplied, g_leftAimed;
float g_aimedPos[3], g_aimedQ[4];

// The eye's world position this frame (the composed camera, not during aim picks): what NPCs look at.
float g_eyeWorld[3];
float g_eyeWorldHist[2][3];
int g_eyeHistI;
bool g_eyeWorldValid;

// The controlled creature is dead (CSWSCreature::GetDead 0x4EF820, fastcall on the server creature).
bool PlayerDead() {
    typedef int(__fastcall * GetDeadFn)(void* creature);
    __try {
        void* c = Weapon_PlayerCreature();
        return c && ((GetDeadFn)0x4EF820)(c) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// The controlled creature's world position ([creature + 0x90]), false if unavailable.
bool CreaturePos(float out[3]) {
    __try {
        BYTE* c = (BYTE*)Weapon_PlayerCreature();
        if (!c) return false;
        memcpy(out, c + 0x90, 3 * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// quaternion (w, x, y, z) -> rotation matrix rows
void QuatWxyzToRows(const float q[4], float m[9]) {
    float w = q[0], x = q[1], y = q[2], z = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w);     m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w);     m[7] = 2 * (y * z + x * w);     m[8] = 1 - 2 * (x * x + y * y);
}

// a (w,x,y,z) * b (w,x,y,z)
void QuatMul(const float a[4], const float b[4], float out[4]) {
    out[0] = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
    out[1] = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
    out[2] = a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
    out[3] = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
}

}  // namespace

// Cutscene / dialogue cameras keep the game's narrow cinematic FOV (~55 deg), and the engine culls the world against
// it: in first person (or looking around a cut) whole walls and floors outside that cone popped in and out. While
// we drive such a camera its FOV is widened to the gameplay 170 deg for culling (the projection is ours anyway) and
// handed back at the start of the next frame, so the game's own state never sees the change. The value written is a
// sentinel (168.5, not 170): the game itself sets 170 when a conversation ends, and restoring over that once left the
// camera stuck in "cutscene" (no turning, endless line-of-sight probes) - 2026-10-26.
static const float kCullFov = 168.5f;
static BYTE* g_fovCam;
static float g_fovSaved;
static bool g_fovPatched;

extern "C" void __cdecl EngineCam_Pre(BYTE* cam) {
    g_applied = false;
    if (g_fovPatched && cam == g_fovCam && *(float*)(cam + 0x1D0) == kCullFov) *(float*)(cam + 0x1D0) = g_fovSaved;
    g_fovPatched = false;
    float fov = *(float*)(cam + 0x1D0);
    if (fov < 25.0f || !VR_IsActive()) return;  // GUI scenes (portraits, menu model) use ~22.7
    float* pos = (float*)(cam + 0x7C);
    float* q = (float*)(cam + 0x88);
    if (!g_verified) {
        if (!g_gaveUp && fabsf(fov - kGameplayFov) < 0.5f) {
            memcpy(g_obsQ, q, sizeof g_obsQ);
            memcpy(g_obsPos, pos, sizeof g_obsPos);
            g_pending = true;
        }
        return;
    }
    KvrPose eye;
    if (!VR_ChooseEye(&eye)) return;
    // the last pick frame left the camera aimed: give the game its own values back (unless it already rewrote them)
    if (g_leftAimed && cam == g_cam && !memcmp(pos, g_aimedPos, sizeof g_aimedPos) && !memcmp(q, g_aimedQ, sizeof g_aimedQ)) {
        memcpy(pos, g_origPos, sizeof g_origPos);
        memcpy(q, g_origQ, sizeof g_origQ);
    }
    g_leftAimed = false;
    KvrPose aim;
    g_pickApplied = VR_AimPick(&aim);
    if (g_pickApplied) eye = aim;  // the hand's aim pose in the same head-relative frame as the eyes

    memcpy(g_origPos, pos, sizeof g_origPos);
    memcpy(g_origQ, q, sizeof g_origQ);
    const float* basePos = g_origPos;
    const float* baseQ = g_origQ;
    bool gameplay = fabsf(fov - kGameplayFov) < 0.5f;
    static int lastMode = -1;  // 1 gameplay, 0 cutscene / dialogue camera
    if ((int)gameplay != lastMode) {
        Log("enginecam: %s (camera fov %.1f)", gameplay ? "gameplay camera" : "cutscene / dialogue camera", fov);
        lastMode = gameplay;
    }
    if (gameplay) {
        // heading: the game camera's for reference, ours for the view
        float M0[9];
        QuatWxyzToRows(g_origQ, M0);
        g_gameYaw = atan2f(-M0[5], -M0[2]);  // forward = -(column z)
        float jx = g_origPos[0] - g_lastPlayPos[0], jy = g_origPos[1] - g_lastPlayPos[1];
        if (!g_viewYawValid || jx * jx + jy * jy > 25.0f) {  // first frame / area change / teleport: start where the game looks
            g_viewYaw = g_gameYaw;
            g_viewYawValid = true;
        }
        memcpy(g_lastPlayPos, g_origPos, sizeof g_lastPlayPos);
        g_viewYaw = WrapPi(g_viewYaw + g_pendingTurn);
        g_pendingTurn = 0;
        static float viewQ[4];
        QuatFromYaw(g_viewYaw, viewQ);
        baseQ = viewQ;
        memcpy(g_playPos, g_origPos, sizeof g_playPos);
        memcpy(g_playQ, viewQ, sizeof g_playQ);
        g_havePlay = true;
        float cp[3];
        if (CreaturePos(cp)) {
            for (int i = 0; i < 3; i++) g_eyeOffset[i] = g_origPos[i] - cp[i];
            g_haveEyeOffset = true;
        }
        if (Features().tracePosition) {
            static DWORD last;
            DWORD t = GetTickCount();
            if (t - last > 2000) {
                last = t;
                // camera forward is local -Z: third column of M(q), negated
                float M0[9];
                QuatWxyzToRows(g_origQ, M0);
                Log("pos: %.1f %.1f %.1f fwd %.2f %.2f view yaw %.0f game yaw %.0f", g_playPos[0], g_playPos[1], g_playPos[2],
                    -M0[2], -M0[5], g_viewYaw * 57.29578f, g_gameYaw * 57.29578f);
            }
        }
    } else if (g_havePlay) {
        // Dialogue / cutscene camera: stay in the character's head unless what the shot shows is out of their line
        // of sight ([Comfort] CutsceneFirstPerson=1: always stay). Each new shot (the scripted camera jumps) gets two
        // hidden probe frames: the shot camera's own view for its subject (the surface at its centre), then the eye
        // toward that subject.
        float sdx = g_origPos[0] - g_shotPos[0], sdy = g_origPos[1] - g_shotPos[1], sdz = g_origPos[2] - g_shotPos[2];
        float qd = fabsf(g_origQ[0] * g_shotQ[0] + g_origQ[1] * g_shotQ[1] + g_origQ[2] * g_shotQ[2] + g_origQ[3] * g_shotQ[3]);
        if (g_shotMode == 0 || sdx * sdx + sdy * sdy + sdz * sdz > 0.5f * 0.5f || qd < 0.996f) {  // a cut (> ~10 deg)
            memcpy(g_shotPos, g_origPos, sizeof g_shotPos);
            memcpy(g_shotQ, g_origQ, sizeof g_shotQ);
            g_shotMode = Features().cutsceneFirstPerson ? 1 : 3;
            if (g_shotMode == 3) {
                float M0[9];
                QuatWxyzToRows(g_shotQ, M0);
                float sf[3] = {-M0[2], -M0[5], -M0[8]};  // the shot's forward (-Z column)
                float rel = fabsf(WrapPi(atan2f(sf[1], sf[0]) - g_viewYaw));  // yaw = atan2(forward y, forward x)
                float cp2[3], toMe[3] = {0, 0, 0}, dm = 1e9f, lookAtMe = -1;
                if (g_haveEyeOffset && CreaturePos(cp2)) {
                    for (int i = 0; i < 3; i++) toMe[i] = cp2[i] + g_eyeOffset[i] - g_shotPos[i];
                    dm = sqrtf(toMe[0] * toMe[0] + toMe[1] * toMe[1] + toMe[2] * toMe[2]);
                    if (dm > 0.01f) lookAtMe = (toMe[0] * sf[0] + toMe[1] * sf[1] + toMe[2] * sf[2]) / dm;
                }
                float headUp = Anim_PlayerHeadHeight();
                if (headUp > -50 && headUp < 1.15f) {  // lying / sitting: any first-person view would be inside the bed
                    // a shot of you lying / sitting (waking in the bunk): your own eyes would be inside the bed -
                    // show the game's shot of you, as it was before
                    g_shotMode = 2;
                    Log("cutscene: you are lying / sitting (head %.2f m up) -> the game's shot", headUp);
                } else if (rel > 1.92f || (lookAtMe > 0.906f && dm < 8.0f)) {  // > 110 deg / within 25 deg of you
                    g_shotMode = 1;
                    Log("cutscene: reverse shot (%.0f deg from your facing%s, head %.2f m up) -> stay first person",
                        rel * 57.29578f, lookAtMe > 0.906f && dm < 8.0f ? ", looking at you" : "", headUp);
                }
            }
        }
        // the character's eye (follows them if the scene walks them somewhere)
        const float* headPos = g_playPos;
        float cp[3];
        if (g_haveEyeOffset && CreaturePos(cp)) {
            for (int i = 0; i < 3; i++) g_followPos[i] = cp[i] + g_eyeOffset[i];
            headPos = g_followPos;
        }
        if (g_shotMode == 4 && VR_LosProbeResult() >= 0) {
            // the shot's subject: its view centre, or 8 m out when it looks at nothing (sky, a far skybox)
            float depth = VR_LosProbeDepth();
            if (depth > 60.0f) depth = 8.0f;
            float M0[9];
            QuatWxyzToRows(g_shotQ, M0);
            for (int i = 0; i < 3; i++) g_subject[i] = g_shotPos[i] - M0[i * 3 + 2] * depth;  // forward = -Z column
            g_shotMode = 5;
        }
        if (g_shotMode == 6) {
            int r = VR_LosProbeResult();
            if (r >= 0) {
                g_shotMode = r ? 1 : 2;
                Log("cutscene: eye -> subject: first hit at %.1f m -> %s", VR_LosProbeDepth(),
                    r ? "visible, stay first person" : "hidden, cut to the game's shot");
            }
        }
        if (g_shotMode == 3 || g_shotMode == 4) {  // probe 1: the shot camera's own view (hidden)
            eye.pos[0] = eye.pos[1] = eye.pos[2] = 0;
            eye.quat[0] = eye.quat[1] = eye.quat[2] = 0;
            eye.quat[3] = 1;
            if (g_shotMode == 3) {
                VR_RequestLosProbe(1e6f);
                g_shotMode = 4;
            }
        } else if (g_shotMode == 5 || g_shotMode == 6) {  // probe 2: from the eye at the subject (hidden)
            float f[3] = {g_subject[0] - headPos[0], g_subject[1] - headPos[1], g_subject[2] - headPos[2]};
            float dist = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            static float probeQ[4];
            QuatLookAlong(f, probeQ);
            basePos = headPos;
            baseQ = probeQ;
            // the subject's bearing against your facing (forward of the view yaw)
            float off = fabsf(WrapPi(atan2f(f[1], f[0]) - g_viewYaw));
            if (false && off > 1.22f) {  // (angle rule retired 2026-10-27: it cut away from shots you could see)
                g_shotMode = 2;
                basePos = g_origPos;
                baseQ = g_origQ;
                Log("cutscene: shot subject %.1f m away, %.0f deg off your facing -> the game's shot", dist,
                    off * 57.29578f);
            } else {
                eye.pos[0] = eye.pos[1] = eye.pos[2] = 0;
                eye.quat[0] = eye.quat[1] = eye.quat[2] = 0;
                eye.quat[3] = 1;
            }
            if (g_shotMode == 5) {
                VR_RequestLosProbe(dist);
                g_shotMode = 6;
                Log("cutscene: shot subject %.1f m from the eye", dist);
            }
        } else if (g_shotMode == 1) {
            basePos = headPos;
            baseQ = g_playQ;
        }
        g_fovSaved = fov;  // cull with the gameplay cone (see above)
        *(float*)(cam + 0x1D0) = kCullFov;
        g_fovCam = cam;
        g_fovPatched = true;
    }
    // Death: instead of the game's death camera (which flung the view up into the air), ease out of your head to a
    // camera 2.4 m behind and 1.7 m above your body, looking at it, and show the body (and hide the VR hands) so you
    // see your character fall. Head tracking still applies on top.
    {
        static double deadSince = -1;
        static float startPos[3], deathPos[3], deathQ[4];
        bool dead = PlayerDead();
        float body[3];
        if (dead && CreaturePos(body)) {
            double now = GetTickCount() / 1000.0;
            if (deadSince < 0) {
                deadSince = now;
                memcpy(startPos, basePos, sizeof startPos);
                Log("enginecam: you died - camera out of your body");
            }
            float t = (float)((now - deadSince) / 1.5);
            if (t > 1) t = 1;
            t = t * t * (3 - 2 * t);  // smoothstep
            float back[3] = {body[0] - cosf(g_viewYaw) * 2.4f, body[1] - sinf(g_viewYaw) * 2.4f, body[2] + 1.7f};
            for (int i = 0; i < 3; i++) deathPos[i] = startPos[i] + (back[i] - startPos[i]) * t;
            float f[3] = {body[0] - deathPos[0], body[1] - deathPos[1], body[2] + 0.5f - deathPos[2]};
            QuatLookAlong(f, deathQ);
            basePos = deathPos;
            baseQ = deathQ;
        } else if (!dead) {
            if (deadSince >= 0) Log("enginecam: alive again - back in first person");
            deadSince = -1;
        }
        VR_SetShowOwnBody(dead);
    }
    if (gameplay) g_shotMode = 0;
    VR_SetCinematic(!gameplay);

    // eye pose is in the camera's local frame (verified to be GL camera space): C_eye = C_base * E
    float M[9];
    QuatWxyzToRows(baseQ, M);
    float eq[4] = {eye.quat[3], eye.quat[0], eye.quat[1], eye.quat[2]};  // host sends x,y,z,w
    float nq[4];
    QuatMul(baseQ, eq, nq);
    for (int r = 0; r < 3; r++)
        pos[r] = basePos[r] + M[r * 3 + 0] * eye.pos[0] + M[r * 3 + 1] * eye.pos[1] + M[r * 3 + 2] * eye.pos[2];
    memcpy(q, nq, sizeof nq);
    if (g_pickApplied) {
        memcpy(g_aimedPos, pos, sizeof g_aimedPos);
        memcpy(g_aimedQ, q, sizeof g_aimedQ);
    }
    g_cam = cam;
    g_applied = true;
    if (!g_pickApplied) {
        memcpy(g_eyeWorldHist[g_eyeHistI], pos, sizeof g_eyeWorld);
        g_eyeHistI ^= 1;
        if (!g_eyeWorldValid) memcpy(g_eyeWorldHist[g_eyeHistI], pos, sizeof g_eyeWorld);
        memcpy(g_eyeWorld, pos, sizeof g_eyeWorld);
        g_eyeWorldValid = true;
    }
    VR_EngineCamApplied(basePos);
}

bool EngineCam_EyeWorld(float out[3]) {
    if (!g_eyeWorldValid) return false;
    // the midpoint of the last two eyes (the game renders one eye per frame, alternately: NPCs looked at the left one)
    for (int i = 0; i < 3; i++) out[i] = (g_eyeWorldHist[0][i] + g_eyeWorldHist[1][i]) * 0.5f;
    return true;
}

bool EngineCam_WorldToTracking(const float w[3], float out[3]) {
    if (!g_havePlay) return false;
    // the gameplay camera base (your head's place and view heading): tracking = M^T (world - base), the inverse of
    // how the eye pose is composed onto it
    float M[9];
    QuatWxyzToRows(g_playQ, M);
    float d[3] = {w[0] - g_playPos[0], w[1] - g_playPos[1], w[2] - g_playPos[2]};
    for (int c = 0; c < 3; c++) out[c] = M[0 * 3 + c] * d[0] + M[1 * 3 + c] * d[1] + M[2 * 3 + c] * d[2];
    return true;
}

extern "C" void __cdecl EngineCam_Post(BYTE* cam) {
    if (g_applied && cam == g_cam) {
        if (g_pickApplied) {
            g_leftAimed = true;  // keep it aimed so this frame's mouse pick goes along the hand
        } else {
            memcpy(cam + 0x7C, g_origPos, sizeof g_origPos);
            memcpy(cam + 0x88, g_origQ, sizeof g_origQ);
        }
    }
    g_applied = false;
}

// Verification: called with the engine's own glMultMatrixf(R) and glTranslatef(-p) for the world camera.
void EngineCam_OnView(const float* m, const float p[3]) {
    if (!g_pending) return;
    g_pending = false;
    float M[9];
    QuatWxyzToRows(g_obsQ, M);
    // expect R = M^T (world -> camera) with the camera's local frame == GL camera space; column-major m[c*4+r] = R[r][c]
    float errRot = 0, errPos = 0;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) errRot = fmaxf(errRot, fabsf(m[c * 4 + r] - M[c * 3 + r]));
    for (int i = 0; i < 3; i++) errPos = fmaxf(errPos, fabsf(p[i] - g_obsPos[i]));
    if (errRot < 2e-3f && errPos < 2e-3f) {
        if (++g_matchFrames == 30) {
            g_verified = true;
            Log("enginecam: convention verified (R = M(q)^T, p = pos) - driving the engine camera from the headset");
        }
        return;
    }
    static int logged;
    if (logged++ < 3) {
        Log("enginecam: MISMATCH rotErr %.4f posErr %.4f | q(w,x,y,z) %.4f %.4f %.4f %.4f pos %.3f %.3f %.3f | p %.3f %.3f %.3f",
            errRot, errPos, g_obsQ[0], g_obsQ[1], g_obsQ[2], g_obsQ[3], g_obsPos[0], g_obsPos[1], g_obsPos[2], p[0], p[1], p[2]);
        Log("enginecam:   M rows %.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f", M[0], M[1], M[2], M[3], M[4], M[5], M[6], M[7], M[8]);
        Log("enginecam:   m cols %.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f", m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
    }
    g_matchFrames = 0;
    if (logged >= 3) {
        g_gaveUp = true;
        Log("enginecam: convention not confirmed; staying on GL-level camera substitution");
    }
}

bool EngineCam_Applied() { return g_applied; }

void EngineCam_Turn(float radians) { g_pendingTurn += radians; }

float EngineCam_ViewYawOffset() { return g_viewYawValid ? WrapPi(g_viewYaw - g_gameYaw) : 0.0f; }

__declspec(naked) static void CamStub() {
    __asm {
        push ecx
        call EngineCam_Pre
        pop ecx
        push ecx
        call dword ptr [g_tramp]
        call EngineCam_Post
        add esp, 4
        ret
    }
}

bool EngineCam_Install() {
    BYTE* target = (BYTE*)(ULONG_PTR)kCamSetupVA;
    if (memcmp(target, kCamSetupOrig, sizeof kCamSetupOrig) != 0) {
        Log("enginecam: unexpected bytes at %08lX, not hooking", kCamSetupVA);
        return false;
    }
    // trampoline: the first 6 bytes (push ebp; mov ebp,esp; and esp,-8) + jmp back to +6
    g_tramp = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(g_tramp, target, 6);
    g_tramp[6] = 0xE9;
    *(INT32*)(g_tramp + 7) = (INT32)((target + 6) - (g_tramp + 11));
    DWORD old;
    VirtualProtect(target, 6, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xE9;
    *(INT32*)(target + 1) = (INT32)((BYTE*)CamStub - (target + 5));
    target[5] = 0x90;
    VirtualProtect(target, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 6);
    Log("enginecam: hooked camera setup at %08lX (verifying convention first)", kCamSetupVA);
    return true;
}
