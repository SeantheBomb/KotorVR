#include "features.h"

#include <windows.h>
#include <cstdio>

extern void Log(const char* fmt, ...);

static FeatureFlags g_flags = {true, true, true, true, false, true, true, true, false, false, 30.0f, 120.0f, false};

const FeatureFlags& Features() { return g_flags; }

static char g_iniPath[MAX_PATH];
const char* Features_IniPath() { return g_iniPath; }

void Features_Load(const char* gameDir) {
    char* path = g_iniPath;
    sprintf_s(g_iniPath, "%s\\kvr\\kvr.ini", gameDir);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        // write the defaults so they are easy to find and flip
        WritePrivateProfileStringA("Features", "EngineCamera", "1", path);
        WritePrivateProfileStringA("Features", "WeaponProxies", "1", path);
        WritePrivateProfileStringA("Features", "Gestures", "1", path);
        WritePrivateProfileStringA("Features", "Combat", "1", path);
        WritePrivateProfileStringA("Features", "Cues", "1", path);
    }
    g_flags.engineCamera = GetPrivateProfileIntA("Features", "EngineCamera", 1, path) != 0;
    g_flags.weaponProxies = GetPrivateProfileIntA("Features", "WeaponProxies", 1, path) != 0;
    g_flags.gestures = GetPrivateProfileIntA("Features", "Gestures", 1, path) != 0;
    g_flags.combat = GetPrivateProfileIntA("Features", "Combat", 1, path) != 0;
    g_flags.hands = GetPrivateProfileIntA("Features", "Hands", 1, path) != 0;
    g_flags.modelCull = GetPrivateProfileIntA("Features", "ModelCull", 1, path) != 0;
    g_flags.cues = GetPrivateProfileIntA("Features", "Cues", 1, path) != 0;
    g_flags.strikeCue = GetPrivateProfileIntA("Features", "StrikeCue", 1, path) != 0;
    g_flags.weaponGallery = GetPrivateProfileIntA("Debug", "WeaponGallery", 0, path) != 0;
    g_flags.snapTurn = GetPrivateProfileIntA("Comfort", "SnapTurn", 0, path) != 0;
    g_flags.snapAngle = (float)GetPrivateProfileIntA("Comfort", "SnapAngle", 30, path);
    g_flags.turnSpeed = (float)GetPrivateProfileIntA("Comfort", "TurnSpeed", 120, path);
    g_flags.cueTest = GetPrivateProfileIntA("Debug", "CueTest", 0, path) != 0;
    g_flags.tracePosition = GetPrivateProfileIntA("Debug", "TracePosition", 0, path) != 0;
    g_flags.telegraphTest = GetPrivateProfileIntA("Debug", "TelegraphTest", 0, path) != 0;
    g_flags.animProbe = GetPrivateProfileIntA("Debug", "AnimProbe", 0, path) != 0;
    g_flags.traceWorld = GetPrivateProfileIntA("Debug", "TraceWorld", 0, path) != 0;
    g_flags.cutsceneFirstPerson = GetPrivateProfileIntA("Comfort", "CutsceneFirstPerson", 0, path) != 0;
    Log("features: engineCamera %d weaponProxies %d gestures %d combat %d (%s)", g_flags.engineCamera,
        g_flags.weaponProxies, g_flags.gestures, g_flags.combat, path);
}
