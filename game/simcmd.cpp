// Debug commands from the test harness, so a scripted run can reach a precise spot and check what happened there
// (2026-11-05: the door / camera / combat repros need the player at exact places, which blind stick pushes in the
// simulator could not reach). kvr_simhost's "game <text>" appends lines to <game>\kvr\simcmd.txt; they are read and
// the file deleted, so nothing happens in normal play (the file never exists).
//
//   goto x y [x y ...]   autopilot: walk through the waypoints (world metres), facing the way it walks.
//                        Logs "autopilot: waypoint N", "autopilot: arrived (x y)" or "autopilot: stuck at (x y)".
//   face x y             turn the view to face a world point ("autopilot: facing (x y)")
//   pos                  log the player's position and the view / game-camera headings
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
#include "pick.h"
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
        g_bestDist = 1e9f;
        g_progressAt = GetTickCount();
        Log("autopilot: goto %d waypoints from (%.2f %.2f)", (int)(g_way.size() / 2), p[0], p[1]);
    } else if (!strcmp(cmd, "face") && nums.size() >= 2) {
        g_faceTo[0] = nums[0];
        g_faceTo[1] = nums[1];
        g_facing = true;
    } else if (!strcmp(cmd, "stop")) {
        g_driving = g_facing = false;
        Log("autopilot: stopped");
    } else if (!strcmp(cmd, "pick")) {
        DWORD id = 0;
        float hp[3] = {};
        bool hit = Pick_Hit(&id, hp);
        Log("simcmd: pickstate %s %08lX at (%.2f %.2f %.2f)", Pick_HitKind(), hit ? id : 0, hp[0], hp[1], hp[2]);
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

void SimCmd_Update(BYTE keys[256]) {
    Poll();
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
    float p[3];
    if (!Weapon_PlayerPos(p)) return;
    float tx = g_way[g_wi * 2], ty = g_way[g_wi * 2 + 1];
    float dx = tx - p[0], dy = ty - p[1], dist = sqrtf(dx * dx + dy * dy);
    if (dist < ((g_wi + 1) * 2 >= g_way.size() ? 0.5f : 0.9f)) {  // pass through the waypoints on the way, stop at the last
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
    if (dist < g_bestDist - 0.3f) {
        g_bestDist = dist;
        g_progressAt = GetTickCount();
    } else if (GetTickCount() - g_progressAt > 3000) {
        g_driving = false;
        Log("autopilot: stuck at (%.2f %.2f) going to (%.2f %.2f), %.1f m short", p[0], p[1], tx, ty, dist);
        return;
    }
    float heading = atan2f(dy, dx);
    TurnToward(heading, 0.08f);
    // the movement keys walk along the game camera: forward / right components of the heading in its frame
    float d = WrapPi(heading - EngineCam_GameYaw());
    float mf = cosf(d), mr = -sinf(d);
    keys[DIK_W] = keys[DIK_S] = keys[DIK_Z] = keys[DIK_C] = 0;
    if (mf > 0.38f) keys[DIK_W] = 0x80;
    if (mf < -0.38f) keys[DIK_S] = 0x80;
    if (mr < -0.38f) keys[DIK_Z] = 0x80;
    if (mr > 0.38f) keys[DIK_C] = 0x80;
}
