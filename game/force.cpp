// Force powers by gesture (v2 pillar 5, docs/V2_PROPOSAL.md).
//
// Gather: hold the left grip - the Force gathers in your hand (a rising hum); how long you gather is the power's
// tier (< 0.7 s first, < 1.6 s second, longer the third - the best rank of that family you know, falling back to
// lower ones). How you let go picks the family, aimed by the left palm (melee.cpp Melee_PalmTarget):
//   shove forward -> Push / Whirlwind / Wave      (both hands shoving at once: the area tier straight away)
//   lift up       -> Choke / Kill / Death Field
//   hold still    -> Stun / Hold / Slow           (droids: Droid Stun / Disable / Destroy)
//   squeeze the left trigger while gathering -> Shock / Lightning / Storm
// At your chest (gather with the left hand on your sternum): hold -> Heal / Cure, push out -> Speed, lift -> Armour /
// Shield / Aura, on yourself.
// Saber Throw: let go of the right grip in the middle of a fast swing - the saber flies at what you point at (Throw /
// Advanced Throw); squeeze again to catch it.
//
// Casting: KotOR's own target action menu (actionring.cpp layout: client [[0x7A39FC]+4]+4 -> +0x40 -> +0x90 = the
// main interface, +0xBC = 3 target columns of 0x38-byte entries, id 0x2000xxxx = Force power xxxx). The target is set
// (CClientExoApp::SetLastTarget), the menu refreshes, the power's entry is selected in its column and the column's key
// performs it - the same path as clicking it, so Force points, saves and animations are all the game's.
// Parries refund a little Force (stats [creature+0xA74]: current +0x124, max via 0x4FD490).
#include "force.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "combat.h"
#include "cues.h"
#include "gestures.h"
#include "melee.h"
#include "vr.h"
#include "weapons.h"

extern void Log(const char* fmt, ...);

namespace {

double Now() { return GetTickCount() / 1000.0; }

BYTE* ActionBlock() {
    __try {
        BYTE* app = *(BYTE**)0x7A39FC;
        BYTE* client = app ? *(BYTE**)(app + 4) : nullptr;
        BYTE* inner = client ? *(BYTE**)(client + 4) : nullptr;
        BYTE* gui = inner ? *(BYTE**)(inner + 0x40) : nullptr;
        BYTE* mi = gui ? *(BYTE**)(gui + 0x90) : nullptr;
        return mi ? mi + 0xBC : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

DWORD PlayerId() {
    __try {
        BYTE* p = (BYTE*)Weapon_PlayerCreature();
        return p ? *(DWORD*)(p + 4) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

// ---- the cast queue
struct Pending {
    int spells[8];
    int n;
    DWORD target;
    double start;
    const char* what;
    bool active;
    bool self;  // a power on yourself: the personal slots
};
Pending g_pending;

// find a power's entry in the target columns; select it; return the column's key (DIK_1..3), 0 if not there
BYTE SelectPower(BYTE* blk, int spell) {
    __try {
        for (int c = 0; c < 3; c++) {
            BYTE* ents = *(BYTE**)(blk + c * 0xC);
            int count = *(int*)(blk + c * 0xC + 4);
            for (int i = 0; ents && i < count && i < 24; i++) {
                int id = *(int*)(ents + i * 0x38 + 8);
                if (id != (0x20000000 | spell)) continue;
                int who = *(signed char*)(blk + 0x1AEA);
                *(int*)(blk + (c + (who + 3) * 3) * 4) = id;
                static const BYTE kKeys[3] = {0x02, 0x03, 0x04};
                return kKeys[c];
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

// the personal action slots (powers on yourself): main interface +0x74 + slot * 0xC (entries, count),
// selected id at +0x1BAC + slot * 4 (CSWGuiMainInterface::SelectPrevPersonalAction 0x6888E0); keys 4..6
BYTE SelectPersonal(BYTE* blk, int spell) {
    __try {
        BYTE* mi = blk - 0xBC;
        for (int s = 0; s < 3; s++) {
            BYTE* ents = *(BYTE**)(mi + 0x74 + s * 0xC);
            int count = *(int*)(mi + 0x78 + s * 0xC);
            for (int i = 0; ents && i < count && i < 32; i++) {
                int id = *(int*)(ents + i * 0x38 + 8);
                if (id != (0x20000000 | spell)) continue;
                *(int*)(mi + 0x1BAC + s * 4) = id;
                static const BYTE kKeys[3] = {0x05, 0x06, 0x07};  // DIK_4..DIK_6
                return kKeys[s];
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

void LogPersonal(BYTE* blk) {
    __try {
        BYTE* mi = blk - 0xBC;
        char line[512];
        int len = sprintf_s(line, "force: your personal actions");
        for (int s = 0; s < 4 && len < 470; s++) {
            BYTE* ents = *(BYTE**)(mi + 0x74 + s * 0xC);
            int count = *(int*)(mi + 0x78 + s * 0xC);
            len += sprintf_s(line + len, sizeof line - len, " | slot %d:", s);
            for (int i = 0; ents && i < count && i < 12 && len < 470; i++)
                len += sprintf_s(line + len, sizeof line - len, " %08X", *(int*)(ents + i * 0x38 + 8));
        }
        Log("%s", line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void LogMenu(BYTE* blk) {
    __try {
        char line[512];
        int len = sprintf_s(line, "force: the target menu has");
        for (int c = 0; c < 6 && len < 470; c++) {
            BYTE* ents = *(BYTE**)(blk + c * 0xC);
            int count = *(int*)(blk + c * 0xC + 4);
            len += sprintf_s(line + len, sizeof line - len, " | col %d:", c);
            for (int i = 0; ents && i < count && i < 10 && len < 470; i++)
                len += sprintf_s(line + len, sizeof line - len, " %08X", *(int*)(ents + i * 0x38 + 8));
        }
        Log("%s", line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void CastSelf(const int* spells, int n, const char* what) {
    memset(&g_pending, 0, sizeof g_pending);
    for (int i = 0; i < n && i < 8; i++) g_pending.spells[i] = spells[i];
    g_pending.n = n;
    g_pending.start = Now();
    g_pending.what = what;
    g_pending.active = true;
    g_pending.self = true;
    Log("force: %s on yourself", what);
}

void Cast(const int* spells, int n, DWORD target, const char* what) {
    if (!target) return;
    memset(&g_pending, 0, sizeof g_pending);
    for (int i = 0; i < n && i < 8; i++) g_pending.spells[i] = spells[i];
    g_pending.n = n;
    g_pending.target = target;
    g_pending.start = Now();
    g_pending.what = what;
    g_pending.active = true;
    Weapon_SetTarget(target | 0x80000000);
    Log("force: %s on %08lX", what, target);
}

// ---- gestures
struct Gather {
    bool on;
    double start;
    float startPos[3];  // left grip (tracking space, y up) when the gather began
    bool trigger;       // the left trigger squeezed while gathering: lightning
    bool chest;         // began at the chest: a buff
    bool pushed;        // shoved forward during the gather
    double lastHum;
};
Gather g_g;
float g_prevR[3];
double g_prevRT = -1;
float g_rSpeedFwd;  // the right hand's forward speed (for two-handed shoves)
bool g_thrown;
double g_thrownAt = -10;
bool g_gripRWas = true;

int Tier(double held) { return held < 0.7 ? 0 : held < 1.6 ? 1 : 2; }

void FamilyCast(const int* tiers, int tier, DWORD target, const char* what) {
    int list[3], n = 0;
    for (int t = tier; t >= 0; t--) list[n++] = tiers[t];  // the best rank you can use, falling back
    if (target) Cast(list, n, target, what);
    else CastSelf(list, n, what);
}

}  // namespace

bool Force_SaberThrown() { return g_thrown; }

void Force_OnParry() {
    typedef short(__fastcall * GetMaxFpFn)(void* creature, void*);
    __try {
        BYTE* c = (BYTE*)Weapon_PlayerCreature();
        BYTE* stats = c ? *(BYTE**)(c + 0xA74) : nullptr;
        if (!stats) return;
        short cur = *(short*)(stats + 0x124);
        short mx = ((GetMaxFpFn)0x4FD490)(c, nullptr);
        if (mx <= 0) return;
        short gain = (short)(mx / 25 > 1 ? mx / 25 : 1);  // 4% of your Force per parry
        short v = cur + gain > mx ? mx : (short)(cur + gain);
        if (v != cur) {
            *(short*)(stats + 0x124) = v;
            Log("force: parry restores %d Force (%d / %d)", v - cur, v, mx);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

BYTE Force_Update(const KvrInput& in, bool fighting) {
    double t = Now();
    BYTE key = 0;
    // ---- the queued cast: wait for the menu to show the target's powers, select, press its column key
    if (g_pending.active) {
        BYTE* blk = ActionBlock();
        double age = t - g_pending.start;
        if (blk && age > 0.05) {
            for (int i = 0; i < g_pending.n && !key; i++) {
                key = g_pending.self ? SelectPersonal(blk, g_pending.spells[i]) : SelectPower(blk, g_pending.spells[i]);
                if (key) Log("force: cast power %d (%s)", g_pending.spells[i], g_pending.what);
            }
        }
        if (key) {
            g_pending.active = false;
            Cues_Fire(CUE_FORCE);
        } else if (age > 0.7) {
            Log("force: %s - none of its powers are known / usable on that target", g_pending.what);
            if (blk) g_pending.self ? LogPersonal(blk) : LogMenu(blk);
            g_pending.active = false;
        }
    }
    if (!(in.gripValid & 1)) return key;
    // head (tracking) for the chest test
    float head[3];
    VR_HeadPos(head);
    const float* lp = in.grip[0].pos;
    // right hand forward speed (two-handed shoves): toward where the head looks, horizontally
    if (in.gripValid & 2) {
        const float* rp = in.grip[1].pos;
        double dt = g_prevRT > 0 ? in.time - g_prevRT : 0;
        if (dt > 0.001 && dt < 0.2) {
            float yaw = VR_HeadYaw();
            float fx = -sinf(yaw), fz = -cosf(yaw);
            g_rSpeedFwd = (float)(((rp[0] - g_prevR[0]) * fx + (rp[2] - g_prevR[2]) * fz) / dt);
        }
        if (dt > 0.001 || g_prevRT < 0) {
            memcpy(g_prevR, rp, 12);
            g_prevRT = in.time;
        }
    }
    // ---- Saber Throw: the right grip let go mid-swing with a lit saber
    bool gripR = in.gripR > 0.5f;
    bool saber = Weapon_Right() == WC_SABER || Weapon_Right() == WC_SABER_SHORT || Weapon_Right() == WC_SABER_DOUBLE;
    if (fighting && saber && g_gripRWas && !gripR && !g_thrown && Melee_BladeSpeed(1) > 3.5f) {
        DWORD target = Melee_PalmTarget();
        DWORD hover;
        if (!target && Weapon_HoverClientId(&hover)) target = hover & 0x7FFFFFFF;
        if (target) {
            static const int kThrow[2] = {49, 4};
            FamilyCast(kThrow, 1, target, "Saber Throw");
            g_thrown = true;
            g_thrownAt = t;
        }
    }
    if (g_thrown && (gripR && !g_gripRWas || t - g_thrownAt > 3.0)) {  // caught (or back by itself)
        g_thrown = false;
        if (gripR) {
            VR_Haptic(1, 1.0f, 0.08f, 120);
            Log("force: saber caught");
        }
    }
    g_gripRWas = gripR;
    // ---- gather with the left grip
    bool gripL = in.gripL > 0.7f;
    if (gripL && !g_g.on) {
        g_g.on = true;
        g_g.start = t;
        memcpy(g_g.startPos, lp, 12);
        g_g.trigger = false;
        g_g.pushed = false;
        float cx = lp[0] - head[0], cy = lp[1] - (head[1] - 0.38f), cz = lp[2] - head[2];
        g_g.chest = cx * cx + cy * cy + cz * cz < 0.22f * 0.22f;
        g_g.lastHum = 0;
    }
    if (g_g.on && gripL) {
        if (in.trigL > 0.6f) g_g.trigger = true;
        double held = t - g_g.start;
        if (t - g_g.lastHum > 0.1) {  // the hum rises as the Force gathers
            g_g.lastHum = t;
            float k = (float)(held > 2.0 ? 1.0 : held / 2.0);
            VR_Haptic(0, 0.15f + 0.5f * k, 0.1f, 60 + 140 * k);
        }
    }
    bool pushed = Gestures_TakeForcePush();  // the shove (also reported outside a gather)
    if (g_g.on && pushed) g_g.pushed = true;  // a shove while gathering counts at the release
    if (g_g.on && !gripL) {  // let go: which family is decided by how
        g_g.on = false;
        double held = t - g_g.start;
        int tier = Tier(held);
        float dy = lp[1] - g_g.startPos[1];
        pushed = pushed || g_g.pushed;
        if (g_g.chest) {  // on yourself (the personal slots)
            static const int kSpeed[3] = {8, 34, 37}, kArmour[3] = {17, 24, 18};
            if (pushed) FamilyCast(kSpeed, tier, 0, "Speed");
            else if (dy > 0.15f) FamilyCast(kArmour, tier, 0, "Armour");
            else {
                // the best buff you know: Heal / Cure / Regeneration, else Aura / Shield / Armour, Speed, Mind
                int list[8] = {28, 10, 39, 18, 24, 17, 37, 22};
                CastSelf(list, 8, "Heal");
            }
            return key;
        }
        if (held < 0.15) return key;  // (the palm has to point at an enemy - that is the gate, not combat mode)
        DWORD target = Melee_PalmTarget();
        if (!target) {
            Log("force: no enemy where your palm points");
            return key;
        }
        if (g_g.trigger) {
            static const int kLightning[3] = {43, 35, 25};
            FamilyCast(kLightning, tier, target, "Lightning");
        } else if (pushed) {
            static const int kPush[3] = {23, 27, 26};
            bool twoHands = g_rSpeedFwd > 1.2f;  // the right hand shoved too: the area power
            FamilyCast(kPush, twoHands ? 2 : tier, target, twoHands ? "Force Wave (two hands)" : "Push");
        } else if (dy > 0.15f) {
            static const int kChoke[3] = {9, 32, 11};
            FamilyCast(kChoke, tier, target, "Choke");
        } else if (held > 0.6) {
            int ordered[6], k = 0;
            static const int kStun[3] = {45, 46, 29};  // by tier: Slow, Stun, Hold
            for (int i = tier; i >= 0; i--) ordered[k++] = kStun[i];
            ordered[k++] = 47;  // droids
            ordered[k++] = 12;
            ordered[k++] = 13;
            Cast(ordered, k, target, "Stun");
        }
        return key;
    }
    if (pushed && !g_g.on) {  // a quick shove without gathering: the first Push you know
        DWORD target = Melee_PalmTarget();
        if (target) {
            static const int kPush[3] = {23, 27, 26};
            FamilyCast(kPush, g_rSpeedFwd > 1.2f ? 2 : 0, target, "Push");
        }
    }
    return key;
}
