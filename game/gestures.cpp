// Motion gestures (x86, inside swkotor.exe). See gestures.h.
//
// Thresholds are first guesses meant to be tuned in the headset; they are per weapon profile where it matters
// (weapons.cpp kProfiles: swingSpeed / swingTravel).

#include "gestures.h"

#include <windows.h>
#include <cmath>
#include <cstring>

#include "../common/kvr_shared.h"
#include "vr.h"
#include "weapons.h"

extern void Log(const char* fmt, ...);

namespace {

struct Sample {
    double t;
    float p[3];
};
const int N = 64;  // ~0.5 s at the game's frame rate, more than any window below

struct Track {
    Sample s[N];
    int head = 0, count = 0;
    void Push(double t, const float* p) {
        if (count && t <= s[(head + N - 1) % N].t) return;  // same host sample twice
        s[head] = {t, {p[0], p[1], p[2]}};
        head = (head + 1) % N;
        if (count < N) count++;
    }
    const Sample& Back(int i) const { return s[(head + N - 1 - i) % N]; }  // 0 = newest
};

Track g_right, g_left, g_head, g_aimDir;
double g_now;

double g_lastSwingAt = -10, g_lastShotAt = -10, g_lastGuardAt = -10, g_lastDodgeAt = -10, g_guardOnAt = -10;
float g_lastAttackQuality;
double g_lastAttackAt = -10;
bool g_swingPending, g_pushPending, g_pullPending;
GestureMove g_move = MOVE_NONE;
double g_moveAt = -10, g_prevSwingAt = -10, g_prevShotAt = -10;
bool g_prevPlainSwing;
const char* kMoveName[] = {"none", "power", "critical", "flurry", "power shot", "rapid"};
float g_swingQuality;
double g_lastPushAt = -10, g_lastPullAt = -10;

float Dist(const float* a, const float* b) {
    float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// peak speed over the newest `window` seconds and straight-line travel across that window
void Motion(const Track& tr, double window, float* peak, float* travel, float* delta3) {
    *peak = 0;
    *travel = 0;
    if (tr.count < 3) return;
    const Sample& newest = tr.Back(0);
    int i = 1;
    for (; i < tr.count; i++) {
        const Sample& a = tr.Back(i - 1);
        const Sample& b = tr.Back(i);
        double dt = a.t - b.t;
        if (dt > 1e-4) *peak = fmaxf(*peak, Dist(a.p, b.p) / (float)dt);
        if (newest.t - b.t >= window) break;
    }
    const Sample& oldest = tr.Back(i < tr.count ? i : tr.count - 1);
    *travel = Dist(newest.p, oldest.p);
    if (delta3)
        for (int k = 0; k < 3; k++) delta3[k] = newest.p[k] - oldest.p[k];
}

void Forward(const float q[4], float out[3]) {  // q * (0,0,-1), q = x,y,z,w
    float x = q[0], y = q[1], z = q[2], w = q[3];
    out[0] = -2 * (x * z + w * y);
    out[1] = -2 * (y * z - w * x);
    out[2] = -(1 - 2 * (x * x + y * y));
}

}  // namespace

double Gestures_Now() { return g_now; }

void Gestures_Update(const KvrInput& in, const float headPos[3]) {
    g_now = in.time > 0 ? in.time : GetTickCount() / 1000.0;
    if (in.gripValid & 2) g_right.Push(g_now, in.grip[1].pos);
    if (in.gripValid & 1) g_left.Push(g_now, in.grip[0].pos);
    g_head.Push(g_now, headPos);
    if (in.gripValid & 2) {
        float f[3];
        Forward(in.grip[1].quat, f);
        g_aimDir.Push(g_now, f);  // unit vector: its speed is angular speed in rad/s
    }

    // --- melee swing (right hand) for melee profiles, including unarmed
    const WeaponProfile& rp = Weapon_Profile(Weapon_Right());
    if (rp.melee && g_now - g_lastSwingAt > 0.45) {
        float peak, travel;
        Motion(g_right, 0.25, &peak, &travel, nullptr);
        // > 25 m/s is a tracking jump (lost / regained controller), not an arm: never a swing
        if (peak > rp.swingSpeed && travel > rp.swingTravel && peak < 25.0f) {
            // what kind of swing: over the last 0.35 s, relative to where the head faces
            float pk, tr, d[3];
            Motion(g_right, 0.35, &pk, &tr, d);
            float yaw = VR_HeadYaw();
            float fwd[2] = {-sinf(yaw), -cosf(yaw)};  // head forward on the floor (x, z)
            float along = d[0] * fwd[0] + d[2] * fwd[1];
            float side = fabsf(d[0] * fwd[1] - d[2] * fwd[0]);
            float startY = g_right.count > 8 ? g_right.Back(g_right.count > 20 ? 20 : g_right.count - 1).p[1] : 0;
            // a clear thrust / overhead chop wins; a flurry is two plain swings in quick succession
            GestureMove move = MOVE_NONE;
            bool plain = false;
            if (along > 0.3f && along > 1.5f * side && fabsf(d[1]) < 0.6f * along) move = MOVE_CRITICAL;
            else if (-d[1] > 0.35f && startY > headPos[1] - 0.05f) move = MOVE_POWER;
            else plain = true;
            if (plain && g_prevPlainSwing && g_now - g_prevSwingAt < 1.0) move = MOVE_FLURRY;
            g_prevPlainSwing = plain && move == MOVE_NONE;
            if (move != MOVE_NONE) {
                g_move = move;
                g_moveAt = g_now;
                Log("gesture: special move %s (along %.2f side %.2f down %.2f start %.2f vs head %.2f)", kMoveName[move],
                    along, side, -d[1], startY, headPos[1]);
            }
            g_prevSwingAt = g_now;
            g_lastSwingAt = g_now;
            g_swingQuality = fminf(2.0f, fmaxf(1.0f, peak / rp.swingSpeed));
            g_swingPending = true;
            g_lastAttackQuality = g_swingQuality;
            g_lastAttackAt = g_now;
            Log("gesture: swing (%s) peak %.2f m/s travel %.2f m quality %.2f", rp.name, peak, travel, g_swingQuality);
        }
    }

    // --- guard: melee weapon raised in front of the body (hand above the waist, ahead of the chest)
    if (rp.melee && Weapon_Right() != WC_NONE && (in.gripValid & 2)) {
        const float* h = in.grip[1].pos;
        float f[3];
        Forward(in.grip[1].quat, f);
        bool raised = h[1] > headPos[1] - 0.55f && h[2] < headPos[2] - 0.15f;
        // blade held across the body: mostly sideways relative to where the head faces, not tilted far up / down and
        // not pointing at the foe (a ready stance with the saber angled up is not a block)
        float yaw = VR_HeadYaw();
        float right[3] = {cosf(yaw), 0, -sinf(yaw)}, fwd[3] = {-sinf(yaw), 0, -cosf(yaw)};
        float lateral = fabsf(f[0] * right[0] + f[2] * right[2]);
        float toward = fabsf(f[0] * fwd[0] + f[2] * fwd[2]);
        bool across = lateral > 0.7f && fabsf(f[1]) < 0.7f && toward < 0.6f;
        if (raised && across) g_lastGuardAt = g_now;
        static bool wasGuard;
        if ((raised && across) != wasGuard) {
            wasGuard = raised && across;
            if (wasGuard) g_guardOnAt = g_now;
            Log("gesture: guard %s (lateral %.2f up %.2f toward %.2f)", wasGuard ? "on" : "off", lateral, f[1], toward);
        }
    }

    // --- dodge: head moves sharply sideways or down (>18 cm within 0.35 s)
    {
        float peak, travel, d[3];
        Motion(g_head, 0.35, &peak, &travel, d);
        if ((fabsf(d[0]) > 0.18f || d[1] < -0.18f) && peak > 0.8f) {
            if (g_now - g_lastDodgeAt > 0.5) Log("gesture: dodge (%.2f, %.2f, %.2f)", d[0], d[1], d[2]);
            g_lastDodgeAt = g_now;
        }
    }

    // --- Force push / pull with the off hand (left): fast thrust forward or yank back
    if (g_now - g_lastPushAt > 0.8 && g_now - g_lastPullAt > 0.8) {
        float peak, travel, d[3];
        Motion(g_left, 0.25, &peak, &travel, d);
        if (peak > 2.0f && travel > 0.28f) {
            if (d[2] < -0.22f) {
                g_pushPending = true;
                g_lastPushAt = g_now;
                Log("gesture: Force push (peak %.2f m/s)", peak);
            } else if (d[2] > 0.22f) {
                g_pullPending = true;
                g_lastPullAt = g_now;
                Log("gesture: Force pull (peak %.2f m/s)", peak);
            }
        }
    }
}

bool Gestures_TakeSwing(float* quality) {
    if (!g_swingPending) return false;
    g_swingPending = false;
    if (quality) *quality = g_swingQuality;
    return true;
}

void Gestures_OnShot() {
    // steadiness: angular speed of the weapon over the 0.3 s before the trigger (rad/s); steady < 0.5 rad/s
    float peak, travel;
    Motion(g_aimDir, 0.3, &peak, &travel, nullptr);
    float q = travel < 0.03f ? 2.0f : travel < 0.08f ? 1.5f : 1.0f;
    float pk2, held;
    Motion(g_aimDir, 1.2, &pk2, &held, nullptr);
    GestureMove move = g_now - g_prevShotAt < 1.0 ? MOVE_RAPID : held < 0.06f ? MOVE_POWER_SHOT : MOVE_NONE;
    if (move != MOVE_NONE) {
        g_move = move;
        g_moveAt = g_now;
        Log("gesture: special move %s (aim drift over 1.2 s %.3f rad)", kMoveName[move], held);
    }
    g_prevShotAt = g_now;
    g_lastShotAt = g_now;
    g_lastAttackQuality = q;
    g_lastAttackAt = g_now;
    Log("gesture: shot, aim drift %.3f rad -> quality %.1f", travel, q);
}

float Gestures_ConsumeRecentAttack(double seconds) {
    if (g_now - g_lastAttackAt > seconds || g_lastAttackQuality <= 0) return 0;
    float q = g_lastAttackQuality;
    g_lastAttackQuality = 0;
    return q;
}

GestureMove Gestures_TakeMove(double seconds) {
    if (g_move == MOVE_NONE || g_now - g_moveAt > seconds) return MOVE_NONE;
    GestureMove m = g_move;
    g_move = MOVE_NONE;
    return m;
}

bool Gestures_Guarding(double seconds) { return g_now - g_lastGuardAt <= seconds; }
bool Gestures_Dodged(double seconds) { return g_now - g_lastDodgeAt <= seconds; }
double Gestures_GuardHeldFor() { return g_now - g_lastGuardAt <= 0.05 ? g_now - g_guardOnAt : -1; }
double Gestures_LastAttackAgo() { return g_now - g_lastAttackAt; }

bool Gestures_TakeForcePush() {
    bool r = g_pushPending;
    g_pushPending = false;
    return r;
}
bool Gestures_TakeForcePull() {
    bool r = g_pullPending;
    g_pullPending = false;
    return r;
}
