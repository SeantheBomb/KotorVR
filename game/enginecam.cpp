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
#include "vr.h"
#include "weapons.h"

extern void Log(const char* fmt, ...);

namespace {

const DWORD kCamSetupVA = 0x45C540;
const BYTE kCamSetupOrig[9] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x68};
const float kGameplayFov = 170.0f;   // camerastyle.2da viewangle in Override
const float kFreezeRadius = 12.0f;

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
// aim picks (VR_BeginAimPick): the camera looks along the hand and is left that way for the game's own mouse picking
bool g_pickApplied, g_leftAimed;
float g_aimedPos[3], g_aimedQ[4];

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

extern "C" void __cdecl EngineCam_Pre(BYTE* cam) {
    g_applied = false;
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
    if (gameplay) {
        memcpy(g_playPos, g_origPos, sizeof g_playPos);
        memcpy(g_playQ, g_origQ, sizeof g_playQ);
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
                Log("pos: %.1f %.1f %.1f fwd %.2f %.2f", g_playPos[0], g_playPos[1], g_playPos[2], -M0[2], -M0[5]);
            }
        }
    } else if (g_havePlay) {
        // dialogue / cutscene camera nearby: stay in the player's head instead of cutting away
        float dx = g_origPos[0] - g_playPos[0], dy = g_origPos[1] - g_playPos[1], dz = g_origPos[2] - g_playPos[2];
        if (dx * dx + dy * dy + dz * dz < kFreezeRadius * kFreezeRadius) {
            basePos = g_playPos;
            baseQ = g_playQ;
            // follow the character if the scene walks them somewhere (the view stays in their head)
            float cp[3];
            if (g_haveEyeOffset && CreaturePos(cp)) {
                for (int i = 0; i < 3; i++) g_followPos[i] = cp[i] + g_eyeOffset[i];
                basePos = g_followPos;
            }
        }
    }
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
    VR_EngineCamApplied(basePos);
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
