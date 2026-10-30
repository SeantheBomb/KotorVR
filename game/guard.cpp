// The block bar (2026-10-30 playtest: the strike-zone diamond was a tiny outline at a fixed spot in front of where the
// game thought you stood - unreadable, and nothing told you when you were lined up or how it went).
//
// Until You Fall's block lines, judged with Arkham Shadow's leniency: for each blow coming at you a glowing bar about
// a saber's length sits on your guard plane - 0.45 m in front of your real chest (the headset, not the character), on
// the attacker's side. Its angle and offset follow the attacker's weapon hand live (the "rhand" weapon hook) - hand high
// = an overhead blow = a level bar above centre; hand out to a side = a side blow = an upright bar on that side - so it
// reads as "that blade, coming here". For the last 0.25 s it holds still. Your blade lying along it (within
// [Combat] BlockTolerance, 25 deg) lights it solid with a soft tone and a haptic tick, and at the impact that is a
// block; swung onto it within [Combat] ParryWindow of the impact, a parry. KotOR's swing animations don't reach your
// real body, so the bar is not a copy of their blade - it is where to put yours.
#include "guard.h"

#include <cmath>
#include <cstdio>
#include <map>

#include "anim.h"
#include "combat.h"
#include "cues.h"
#include "enginecam.h"
#include "features.h"
#include "hover.h"
#include "melee.h"
#include "vr.h"
#include "weapons.h"

extern void Log(const char* fmt, ...);

namespace {

const float kHalf = 0.28f;      // half the bar's length
const float kForward = 0.45f;   // guard plane in front of the chest
const float kOffset = 0.18f;    // bar centre shifted toward the blow's side
const float kFreeze = 0.25f;    // seconds before the impact it holds still
const float kMinCos = 0.906f;   // 25 deg

struct State {
    double seen;
    float s[2];           // the blow's side on the guard plane (smoothed unit vector: x right, y up)
    bool sInit;
    GuardBar bar;
    bool lined;
    double linedSince;    // when the blade came onto it
    double lastLined;     // when it was last on it
    float speedAtLine;
    double resultAt;
    int result;
};
std::map<DWORD, State> g_state;
float g_tolerance = 0.12f, g_parryWindow = 0.3f;
bool g_settingsRead;

double Now() { return GetTickCount() / 1000.0; }

void ReadSettings() {
    g_settingsRead = true;
    const char* ini = Features_IniPath();
    char v[32];
    GetPrivateProfileStringA("Combat", "BlockTolerance", "0.12", v, sizeof v, ini);
    float f;
    if (sscanf_s(v, "%f", &f) == 1 && f > 0.02f && f < 0.6f) g_tolerance = f;
    GetPrivateProfileStringA("Combat", "ParryWindow", "0.3", v, sizeof v, ini);
    if (sscanf_s(v, "%f", &f) == 1 && f > 0.05f && f < 1.5f) g_parryWindow = f;
    Log("guard: block bar tolerance %.2f m, parry window %.2f s", g_tolerance, g_parryWindow);
}

void Label(const GuardBar& b, const char* text) {
    float mid[3] = {(b.a[0] + b.b[0]) * 0.5f, (b.a[1] + b.b[1]) * 0.5f, (b.a[2] + b.b[2]) * 0.5f + 0.22f}, pos[3];
    if (!EngineCam_WorldToTracking(mid, pos)) return;
    float head[3];
    VR_HeadPos(head);
    float yaw = atan2f(head[0] - pos[0], head[2] - pos[2]);
    float q[4] = {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
    Hover_Suppress(0.8);
    VR_ShowText(3, KVR_ANCHOR_WORLD, pos, q, 0.16f, text, -1);
}

}  // namespace

void Guard_Update() {
    if (!g_settingsRead) ReadSettings();
    double t = Now();
    CombatWarning w[8];
    int n = Combat_IncomingWarnings(w, 8);
    float eye[3];
    bool haveEye = EngineCam_EyeWorld(eye);
    bool melee = Weapon_Right() != WC_NONE && Weapon_Profile(Weapon_Right()).melee;
    for (int i = 0; i < n && haveEye && melee; i++) {
        if (w[i].ranged) continue;
        float ap[3];
        if (!Weapon_ObjectPos(w[i].attacker, ap)) continue;
        State& st = g_state[w[i].attacker];
        if (t - st.seen > 0.5) {  // a new blow from this attacker
            st = State{};
            Log("guard: bar for the blow from %08lX (%.2f s left)", w[i].attacker, w[i].secondsLeft);
        }
        st.seen = t;
        GuardBar& b = st.bar;
        b.attacker = w[i].attacker;
        b.secondsLeft = w[i].secondsLeft;
        b.total = w[i].total;
        b.result = st.result;
        b.resultAge = st.result ? (float)(t - st.resultAt) : 0;
        if (!b.frozen && st.result == 0) {
            float chest[3] = {eye[0], eye[1], eye[2] - 0.4f};
            float ux = ap[0] - chest[0], uy = ap[1] - chest[1], ul = sqrtf(ux * ux + uy * uy);
            if (ul < 0.05f) continue;
            ux /= ul;
            uy /= ul;
            float rx = uy, ry = -ux;  // your right, facing the attacker
            // the blow's side: the attacker's weapon hand against its chest, on your guard plane
            float side[2] = {0, 1};  // default: overhead
            b.haveHand = Anim_PartPos(Anim_ModelAt(ap), "rhand", b.hand);
            if (b.haveHand) {
                float hx = (b.hand[0] - ap[0]) * rx + (b.hand[1] - ap[1]) * ry, hz = b.hand[2] - (ap[2] + 1.2f);
                float hl = sqrtf(hx * hx + hz * hz);
                if (hl > 0.12f) {
                    side[0] = hx / hl;
                    side[1] = hz / hl;
                }
            }
            if (!st.sInit) {
                st.s[0] = side[0];
                st.s[1] = side[1];
                st.sInit = true;
            } else {  // smoothed (~0.08 s)
                st.s[0] += (side[0] - st.s[0]) * 0.2f;
                st.s[1] += (side[1] - st.s[1]) * 0.2f;
                float l = sqrtf(st.s[0] * st.s[0] + st.s[1] * st.s[1]) + 1e-5f;
                st.s[0] /= l;
                st.s[1] /= l;
            }
            float c[3] = {chest[0] + ux * kForward + (rx * st.s[0]) * kOffset, chest[1] + uy * kForward + (ry * st.s[0]) * kOffset,
                          chest[2] + st.s[1] * kOffset};
            // along the bar: perpendicular to the side, in the plane (side (x, z) -> axis (-z, x))
            float ax[3] = {rx * -st.s[1], ry * -st.s[1], st.s[0]};
            for (int k = 0; k < 3; k++) {
                b.a[k] = c[k] - ax[k] * kHalf;
                b.b[k] = c[k] + ax[k] * kHalf;
            }
            if (w[i].secondsLeft < kFreeze) b.frozen = true;
        }
        float speed = 0;
        bool on = st.result == 0 && Melee_BladeOnBar(b.a, b.b, g_tolerance, kMinCos, &speed);
        if (on) st.lastLined = t;
        if (on && !st.lined) {
            Log("guard: blade on the bar for %08lX (%.2f s left, %.1f m/s)", w[i].attacker, w[i].secondsLeft, speed);
            st.linedSince = t;
            st.speedAtLine = speed;
            VR_Haptic(1, 0.25f, 0.02f, 180);
            Cues_Tone("gui_prompt", 0.3f, 1.4f);
        }
        st.lined = on;
        b.lined = on;
    }
    for (auto it = g_state.begin(); it != g_state.end();)
        it = t - it->second.seen > 1.0 ? g_state.erase(it) : ++it;
}

int Guard_Bars(GuardBar* out, int max) {
    int n = 0;
    double t = Now();
    for (auto& kv : g_state) {
        if (n >= max) break;
        if (t - kv.second.seen > 0.2 && !(kv.second.result && t - kv.second.resultAt < 0.6)) continue;
        out[n] = kv.second.bar;
        out[n].result = kv.second.result;
        out[n].resultAge = kv.second.result ? (float)(t - kv.second.resultAt) : 0;
        n++;
    }
    return n;
}

bool Guard_Judge(DWORD attacker, bool* parry, float* speed, bool* haveBar) {
    *parry = false;
    *speed = 0;
    auto it = g_state.find(attacker);
    double t = Now();
    *haveBar = it != g_state.end() && t - it->second.seen < 0.3;
    if (!*haveBar) return false;
    State& st = it->second;
    if (t - st.lastLined > 0.12) return false;  // a hair of forgiveness: on it in the last 0.12 s
    Melee_BladeOnBar(st.bar.a, st.bar.b, g_tolerance * 1.5f, 0.5f, speed);
    // a parry: you swung onto it shortly before the impact, not just held it there
    *parry = t - st.linedSince <= g_parryWindow && (st.speedAtLine >= 1.2f || *speed >= 1.6f);
    return true;
}

void Guard_Result(DWORD attacker, int result) {
    auto it = g_state.find(attacker);
    if (it == g_state.end()) return;
    State& st = it->second;
    st.result = result;
    st.resultAt = Now();
    st.seen = st.resultAt;
    Label(st.bar, result == 2 ? "~#PARRY" : result == 1 ? "~BLOCK" : Now() - st.lastLined < 0.6 ? "~TOO EARLY" : "~MISSED");
    Log("guard: %08lX %s", attacker, result == 2 ? "parried" : result == 1 ? "blocked" : "missed");
}
