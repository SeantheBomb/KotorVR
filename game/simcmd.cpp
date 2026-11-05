// Debug commands from the test harness, so a scripted run can reach a precise spot and check what happened there
// (2026-11-05: the door / camera / combat repros need the player at exact places, which blind stick pushes in the
// simulator could not reach). kvr_simhost's "game <text>" appends lines to <game>\kvr\simcmd.txt; they are read and
// the file deleted, so nothing happens in normal play (the file never exists).
//
//   goto x y [x y ...]   autopilot: walk through the waypoints (world metres), facing the way it walks.
//                        Logs "autopilot: waypoint N", "autopilot: arrived (x y)" or "autopilot: stuck at (x y)".
//   face x y             turn the view to face a world point ("autopilot: facing (x y)")
//   approach d           walk to the nearest enemy, stop d metres from it ("autopilot: arrived ... from the enemy")
//   faceenemy            turn the view to the nearest live enemy
//   facewall             face the nearest wall 2..12 m away (and aim at it)
//   aimhint              log how far to turn the right hand so its gun barrel points at what faceenemy /
//                        facewall chose
//   fightstep            one step of kvr_simhost "fight": close in on / face the nearest enemy ("fight: ...")
//   passdoor x y         walk through the doorway to 1.6 m past it (arrived = open)
//   doorcheck x y        "simcmd: door at (x y) open|shut" (line of sight through the doorway)
//   pointat x y z / pointhint   face a point / how far to turn the hand so its pointing ray is on it
//   posmark / moved m    remember the position / log "moved ok" if the player has gone m metres since
//   pos                  log the player's position and the view / game-camera headings
//   god 1|0              the controlled character's hit points never drop (the test rig's invulnerability)
//   neareye 1|0          log frames that draw world geometry within 12 cm of the eye ("neareye: ...")
//   trace                a GL trace of the next world frame (kvr/trace_f<frame>.txt)
//   doors                log the area's doors and how many of the usable ones the ray can pick
//   pick                 log what the right hand's ray picks right now ("simcmd: pickstate door ...")
//   stop                 stop the autopilot
//
// Movement goes through the same keys the left stick uses (W/S/Z/C along the game camera), so it walks, collides and
// slides along walls exactly as the player does.
#include "simcmd.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dinput.h>

#include "enginecam.h"
#include "melee.h"
#include "pick.h"
#include "dialogue.h"
#include "../common/kvr_shared.h"
#include "vr.h"
#include "weapons.h"

extern void Log(const char* fmt, ...);

namespace {

char g_path[MAX_PATH];
DWORD g_pollAt;

std::vector<float> g_way;  // x, y pairs
size_t g_wi;
bool g_driving;
float g_bestDist;
DWORD g_progressAt;
bool g_facing;
float g_faceTo[2];
float g_posMark[3];
float g_aimPoint[3];  // what aimhint aims the gun at (faceenemy / facewall set it)
bool g_aimSet;
float g_pointAt[3];
float g_pointFrom[3];  // where you stood when the last pointat began (the near side of a door)  // pointat: what the hand's pointing ray should be on (pointhint)
float g_approachTo;  // > 0: the goto is chasing the nearest enemy and stops this close to it
DWORD g_approachId;
bool g_approachEnemy;  // the approach follows the nearest enemy (else a fixed point: pointat)
static bool g_god;
static short g_godHp;

float WrapPi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

// turn the view toward a world heading, at most `maxStep` radians this frame; true when within 3 degrees
bool TurnToward(float heading, float maxStep) {
    float err = WrapPi(heading - EngineCam_ViewYaw());
    float step = err > maxStep ? maxStep : err < -maxStep ? -maxStep : err;
    if (fabsf(step) > 1e-4f) EngineCam_Turn(step);
    return fabsf(err) < 3.0f * 3.14159265f / 180.0f;
}

void Run(char* line) {
    char* ctx = nullptr;
    char* cmd = strtok_s(line, " \t\r\n", &ctx);
    if (!cmd) return;
    std::vector<float> nums;
    for (char* t = strtok_s(nullptr, " \t\r\n", &ctx); t; t = strtok_s(nullptr, " \t\r\n", &ctx))
        nums.push_back((float)atof(t));
    float p[3] = {};
    bool havePos = Weapon_PlayerPos(p);
    if (!strcmp(cmd, "goto") && nums.size() >= 2) {
        g_way.assign(nums.begin(), nums.end() - (nums.size() & 1));
        g_wi = 0;
        g_driving = true;
        g_facing = false;
        g_approachTo = 0;  // a route, not a fight's or pointat's approach (that would keep steering at its target)
        g_bestDist = 1e9f;
        g_progressAt = GetTickCount();
        Log("autopilot: goto %d waypoints from (%.2f %.2f)", (int)(g_way.size() / 2), p[0], p[1]);
    } else if (!strcmp(cmd, "god")) {
        g_god = nums.empty() || nums[0] != 0;
        g_godHp = 0;
        Log("simcmd: god %s", g_god ? "on" : "off");
    } else if (!strcmp(cmd, "face") && nums.size() >= 2) {
        g_faceTo[0] = nums[0];
        g_faceTo[1] = nums[1];
        g_facing = true;
    } else if (!strcmp(cmd, "stop")) {
        g_driving = g_facing = false;
        g_approachTo = 0;
        Log("autopilot: stopped");
    } else if (!strcmp(cmd, "pick")) {
        DWORD id = 0;
        float hp[3] = {};
        bool hit = Pick_Hit(&id, hp);
        Log("simcmd: pickstate %s %08lX at (%.2f %.2f %.2f)", Pick_HitKind(), hit ? id : 0, hp[0], hp[1], hp[2]);
    } else if (!strcmp(cmd, "approach") && nums.size() >= 1) {
        float e[3];
        g_approachId = havePos ? Melee_NearestEnemy(p, e) : 0;
        if (g_approachId) {
            g_way.assign({e[0], e[1]});
            g_wi = 0;
            g_driving = true;
            g_facing = false;
            g_bestDist = 1e9f;
            g_progressAt = GetTickCount();
            g_approachTo = nums[0];
            g_approachEnemy = true;
            Log("autopilot: approaching %08lX to %.1f m", g_approachId, nums[0]);
        } else {
            Log("simcmd: no enemy to approach");
        }
    } else if (!strcmp(cmd, "faceenemy")) {
        float e[3];
        DWORD id = havePos ? Melee_NearestEnemy(p, e) : 0;
        if (id) {
            g_faceTo[0] = e[0];
            g_faceTo[1] = e[1];
            g_facing = true;
            g_aimPoint[0] = e[0];
            g_aimPoint[1] = e[1];
            g_aimPoint[2] = e[2] + 1.15f;  // chest
            g_aimSet = true;
            Log("simcmd: nearest enemy %08lX at (%.2f %.2f), %.1f m", id, e[0], e[1],
                sqrtf((e[0] - p[0]) * (e[0] - p[0]) + (e[1] - p[1]) * (e[1] - p[1])));
        } else {
            Log("simcmd: no enemy");
        }
    } else if (!strcmp(cmd, "aimhint")) {
        // how far to turn the right hand (yaw left+, pitch up+, degrees) so the barrel points at the nearest
        // enemy's chest; yaw / pitch changes of the hand turn the barrel the same way (simhost hand_adjust)
        float bo[3], bd[3];
        DWORD id = g_aimSet ? 1 : 0;
        if (g_aimSet && Weapons_Barrel(bo, bd)) {
            float t[3] = {g_aimPoint[0] - bo[0], g_aimPoint[1] - bo[1], g_aimPoint[2] - bo[2]};
            float tl = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
            float dyaw = WrapPi(atan2f(t[1], t[0]) - atan2f(bd[1], bd[0])) * 57.29578f;
            float dpitch = (asinf(t[2] / tl) - asinf(bd[2] > 1 ? 1 : bd[2] < -1 ? -1 : bd[2])) * 57.29578f;
            Log("simcmd: aimhint dyaw=%.2f dpitch=%.2f (%.1f m)", dyaw, dpitch, tl);
        } else {
            Log("simcmd: aimhint none (target %d, barrel %d)", (int)id, Weapons_Barrel(bo, bd) ? 1 : 0);
        }
    } else if (!strcmp(cmd, "facewall")) {
        // the nearest wall 2..12 m away (16 headings, the area's line of sight at chest height): face it, aim at it
        DWORD pid = 0;
        if (void* pc = Weapon_PlayerCreature()) pid = *(DWORD*)((BYTE*)pc + 4);
        float best = 1e9f, bh = 0;
        float o[3] = {p[0], p[1], p[2] + 1.15f};
        for (int k = 0; havePos && k < 16; k++) {
            float h = k * 6.2831853f / 16, dir[3] = {cosf(h), sinf(h), 0};
            float lo = 0, hi = 12;
            float e[3] = {o[0] + dir[0] * hi, o[1] + dir[1] * hi, o[2]};
            if (Melee_LineOfSight(o, e, pid, 0)) continue;
            for (int i = 0; i < 10; i++) {
                float m = (lo + hi) * 0.5f, q[3] = {o[0] + dir[0] * m, o[1] + dir[1] * m, o[2]};
                if (Melee_LineOfSight(o, q, pid, 0)) lo = m;
                else hi = m;
            }
            if (hi >= 2.0f && hi < best) {
                best = hi;
                bh = h;
            }
        }
        if (best < 1e8f) {
            g_aimPoint[0] = o[0] + cosf(bh) * best;
            g_aimPoint[1] = o[1] + sinf(bh) * best;
            g_aimPoint[2] = o[2];
            g_aimSet = true;
            g_faceTo[0] = g_aimPoint[0];
            g_faceTo[1] = g_aimPoint[1];
            g_facing = true;
            Log("simcmd: wall %.1f m away at heading %.0f", best, bh * 57.29578f);
        } else {
            Log("simcmd: no wall within 12 m");
        }
    } else if (!strcmp(cmd, "fightstep")) {
        // one step of a scripted fight: close in on the nearest enemy, face it, say how to attack (kvr_simhost fight)
        float e[3];
        DWORD id = havePos ? Melee_NearestEnemy(p, e) : 0;
        float d = id ? sqrtf((e[0] - p[0]) * (e[0] - p[0]) + (e[1] - p[1]) * (e[1] - p[1])) : 1e9f;
        bool melee = Weapon_Profile(Weapon_Right()).melee || Weapon_Right() == WC_NONE;
        if (!id || d > 25.0f) {
            g_driving = g_facing = false;
            g_approachTo = 0;
            Log("fight: clear");
        } else if (d > (melee ? 1.6f : 14.0f)) {
            if (!g_driving) {
                g_way.assign({e[0], e[1]});
                g_wi = 0;
                g_driving = true;
                g_facing = false;
                g_bestDist = 1e9f;
                g_progressAt = GetTickCount();
                g_approachTo = melee ? 1.2f : 10.0f;
                g_approachEnemy = true;
            }
            Log("fight: closing on %08lX, %.1f m, at (%.2f %.2f) from (%.2f %.2f)", id, d, e[0], e[1], p[0], p[1]);
        } else {
            g_driving = false;
            g_approachTo = 0;
            g_faceTo[0] = e[0];
            g_faceTo[1] = e[1];
            g_facing = true;
            g_aimPoint[0] = e[0];
            g_aimPoint[1] = e[1];
            g_aimPoint[2] = e[2] + 1.15f;
            g_aimSet = true;
            Log("fight: engaged %s %08lX, %.1f m", melee ? "melee" : "ranged", id, d);
        }
    } else if (!strcmp(cmd, "pointatnpc")) {  // pointat the nearest friendly creature's chest
        float f[3];
        if (Pick_NearestFriend(f)) {
            char buf[96];
            sprintf_s(buf, "pointat %.2f %.2f %.2f", f[0], f[1], f[2] + 1.3f);
            Log("simcmd: nearest friend at (%.2f %.2f)", f[0], f[1]);
            std::vector<char> w(buf, buf + strlen(buf) + 1);
            Run(w.data());
        } else {
            Log("simcmd: no friend near");
        }
    } else if (!strcmp(cmd, "passdoor") && nums.size() >= 2) {
        // walk through the doorway at (x, y) to 1.6 m past it: "autopilot: arrived" = it is open
        // the far side: away from where you stood when you went to use it (the game may have stepped you closer)
        float dx = nums[0] - g_pointFrom[0], dy = nums[1] - g_pointFrom[1], dl = sqrtf(dx * dx + dy * dy) + 1e-4f;
        float n[2] = {dx / dl, dy / dl};
        if (Pick_DoorNormal(nums[0], nums[1], n)) {
            if (n[0] * dx + n[1] * dy < 0) n[0] = -n[0], n[1] = -n[1];
        }
        g_way.assign({nums[0], nums[1], nums[0] + n[0] * 1.6f, nums[1] + n[1] * 1.6f});
        g_wi = 0;
        g_driving = true;
        g_facing = false;
        g_approachTo = 0;
        g_bestDist = 1e9f;
        g_progressAt = GetTickCount();
        Log("autopilot: through the door at (%.2f %.2f)", nums[0], nums[1]);
    } else if (!strcmp(cmd, "doorcheck") && nums.size() >= 2) {
        // is the doorway at (x, y) open? the area's line of sight from your side to 1.5 m past it (a shut door blocks)
        DWORD pid = 0;
        if (void* pc = Weapon_PlayerCreature()) pid = *(DWORD*)((BYTE*)pc + 4);
        float dx = nums[0] - p[0], dy = nums[1] - p[1], dl = sqrtf(dx * dx + dy * dy) + 1e-4f;
        float from[3] = {p[0], p[1], p[2] + 1.2f}, to[3] = {nums[0] + dx / dl * 1.5f, nums[1] + dy / dl * 1.5f, p[2] + 1.2f};
        bool open = havePos && Melee_LineOfSight(from, to, pid, 0);
        Log("simcmd: door at (%.2f %.2f) %s", nums[0], nums[1], open ? "open" : "shut");
    } else if (!strcmp(cmd, "pointat") && nums.size() >= 3) {
        // walk up to it (an action never walks you there yourself in VR), then face it
        memcpy(g_pointFrom, p, sizeof g_pointFrom);
        g_pointAt[0] = nums[0];
        g_pointAt[1] = nums[1];
        g_pointAt[2] = nums[2];
        g_faceTo[0] = nums[0];
        g_faceTo[1] = nums[1];
        float dx = nums[0] - p[0], dy = nums[1] - p[1];
        if (havePos && dx * dx + dy * dy > 1.4f * 1.4f) {
            g_way.assign({nums[0], nums[1]});
            g_wi = 0;
            g_driving = true;
            g_facing = false;
            g_bestDist = 1e9f;
            g_progressAt = GetTickCount();
            g_approachTo = 1.1f;
            g_approachEnemy = false;
            Log("autopilot: walking up to (%.2f %.2f)", nums[0], nums[1]);
        } else {
            g_facing = true;
        }
    } else if (!strcmp(cmd, "pointhint")) {
        // like aimhint, for the hand's pointing ray (what grip acts on) to the pointat point
        float o[3], d[3];
        if (Pick_Aim(o, d)) {
            float t[3] = {g_pointAt[0] - o[0], g_pointAt[1] - o[1], g_pointAt[2] - o[2]};
            float tl = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]) + 1e-5f;
            float dyaw = WrapPi(atan2f(t[1], t[0]) - atan2f(d[1], d[0])) * 57.29578f;
            float dpitch = (asinf(t[2] / tl) - asinf(d[2] > 1 ? 1 : d[2] < -1 ? -1 : d[2])) * 57.29578f;
            Log("simcmd: aimhint dyaw=%.2f dpitch=%.2f (pointing, %.1f m)", dyaw, dpitch, tl);
        } else {
            Log("simcmd: aimhint none (no pointing ray)");
        }
    } else if (!strcmp(cmd, "posmark")) {
        memcpy(g_posMark, p, sizeof g_posMark);
    } else if (!strcmp(cmd, "moved") && nums.size() >= 1) {
        float d = sqrtf((p[0] - g_posMark[0]) * (p[0] - g_posMark[0]) + (p[1] - g_posMark[1]) * (p[1] - g_posMark[1]));
        Log("simcmd: moved %.2f m since posmark -> %s", d, d >= nums[0] ? "moved ok" : "moved SHORT");
    } else if (!strcmp(cmd, "neareye")) {  // "neareye 1" / "neareye 0" (numbers: the parser takes numbers)
        VR_SetNearEyeWatch(!nums.empty() && nums[0] != 0);
        Log("simcmd: near-eye watch %s", !nums.empty() && nums[0] != 0 ? "on" : "off");
    } else if (!strcmp(cmd, "trace")) {
        VR_TraceNextWorldFrame();
    } else if (!strcmp(cmd, "doors")) {
        Pick_LogDoors();
    } else if (!strcmp(cmd, "pos")) {
        Log("simcmd: player %s (%.2f %.2f %.2f) view yaw %.1f game yaw %.1f", havePos ? "at" : "unknown", p[0], p[1],
            p[2], EngineCam_ViewYaw() * 57.29578f, EngineCam_GameYaw() * 57.29578f);
    } else {
        Log("simcmd: unknown command '%s'", cmd);
    }
}

void Poll() {
    DWORD now = GetTickCount();
    if (now - g_pollAt < 200) return;
    g_pollAt = now;
    if (!g_path[0] || GetFileAttributesA(g_path) == INVALID_FILE_ATTRIBUTES) return;
    FILE* f = nullptr;
    if (fopen_s(&f, g_path, "r") || !f) return;
    std::vector<std::string> lines;
    char buf[512];
    while (fgets(buf, sizeof buf, f)) lines.push_back(buf);
    fclose(f);
    DeleteFileA(g_path);
    for (auto& l : lines) {
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        Log("simcmd: %s", l.c_str());
        std::vector<char> w(l.begin(), l.end());
        w.push_back(0);
        Run(w.data());
    }
}

}  // namespace

void SimCmd_Init(const char* gameDir) {
    sprintf_s(g_path, "%s\\kvr\\simcmd.txt", gameDir);
    DeleteFileA(g_path);  // a stale file from an aborted run must not drive a real session
}

bool SimCmd_Driving() { return g_driving; }

// "god 1": the test rig's own invulnerability (the cheat console's is a toggle, and a save or a module change flips
// or drops it): the controlled character's hit points never go below where they were when it came on
static void GodTick() {
    BYTE* c = (BYTE*)Weapon_PlayerCreature();
    if (!g_god || !c) return;
    __try {
        short& hp = *(short*)(c + 0xDC);  // current hit points (see combat.cpp PostureOf)
        if (hp > g_godHp) g_godHp = hp;
        if (hp < g_godHp) hp = g_godHp;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SimCmd_Update(BYTE keys[256]) {
    Poll();
    GodTick();
    if (g_facing && !g_driving) {
        float p[3];
        if (Weapon_PlayerPos(p)) {
            float h = atan2f(g_faceTo[1] - p[1], g_faceTo[0] - p[0]);
            if (TurnToward(h, 0.06f)) {
                g_facing = false;
                Log("autopilot: facing (%.2f %.2f)", g_faceTo[0], g_faceTo[1]);
            }
        }
    }
    if (!g_driving) return;
    // a conversation or cutscene holds the character: wait it out (it is not being stuck)
    const KvrShared* shm = VR_Shared();
    if (Dialogue_Showing() || (shm && shm->cinematic)) {
        g_progressAt = GetTickCount();
        return;
    }
    float p[3];
    if (!Weapon_PlayerPos(p)) return;
    if (g_approachTo > 0) {
        float e[3];
        if (g_approachEnemy && Melee_NearestEnemy(p, e)) {
            g_way[0] = e[0];
            g_way[1] = e[1];
        }
        float ex = g_way[0] - p[0], ey = g_way[1] - p[1];
        if (ex * ex + ey * ey < g_approachTo * g_approachTo) {
            g_driving = false;
            g_approachTo = 0;
            Log("autopilot: arrived (%.2f %.2f), %.1f m from the %s", p[0], p[1], sqrtf(ex * ex + ey * ey),
                g_approachEnemy ? "enemy" : "target");
            if (!g_approachEnemy) g_facing = true;  // then turn to it (pointat)
            return;
        }
    }
    float tx = g_way[g_wi * 2], ty = g_way[g_wi * 2 + 1];
    float dx = tx - p[0], dy = ty - p[1], dist = sqrtf(dx * dx + dy * dy);
    if (dist < ((g_wi + 1) * 2 >= g_way.size() ? 0.2f : 0.9f)) {  // pass through the waypoints on the way, stop at the last
        g_wi++;
        g_bestDist = 1e9f;
        g_progressAt = GetTickCount();
        if (g_wi * 2 >= g_way.size()) {
            g_driving = false;
            Log("autopilot: arrived (%.2f %.2f)", p[0], p[1]);
            return;
        }
        Log("autopilot: waypoint %d (%.2f %.2f)", (int)g_wi, p[0], p[1]);
        return;
    }
    static int detours;          // sidesteps tried since the last progress (a companion in a doorway)
    static DWORD detourUntil;
    static float detourSide = 1;
    if (dist < g_bestDist - 0.3f) {
        g_bestDist = dist;
        g_progressAt = GetTickCount();
        detours = 0;
    } else if (GetTickCount() - g_progressAt > 1500 && GetTickCount() > detourUntil) {
        if (detours >= 4) {
            g_driving = false;
            detours = 0;
            Log("autopilot: stuck at (%.2f %.2f) going to (%.2f %.2f), %.1f m short", p[0], p[1], tx, ty, dist);
            return;
        }
        detours++;
        detourSide = -detourSide;
        detourUntil = GetTickCount() + 700;
        g_progressAt = GetTickCount();
        Log("autopilot: blocked - sidestep %s", detourSide > 0 ? "left" : "right");
    }
    float heading = atan2f(dy, dx);
    TurnToward(heading, 0.08f);
    if (GetTickCount() < detourUntil) heading += detourSide * 1.57f;  // step aside, then on again
    // the movement keys walk along the game camera: forward / right components of the heading in its frame
    float d = WrapPi(heading - EngineCam_GameYaw());
    float mf = cosf(d), mr = -sinf(d);
    keys[DIK_W] = keys[DIK_S] = keys[DIK_Z] = keys[DIK_C] = 0;
    if (mf > 0.38f) keys[DIK_W] = 0x80;
    if (mf < -0.38f) keys[DIK_S] = 0x80;
    if (mr < -0.38f) keys[DIK_Z] = 0x80;
    if (mr > 0.38f) keys[DIK_C] = 0x80;
}
