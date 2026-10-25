#include "features.h"

#include <windows.h>
#include <cstdio>

extern void Log(const char* fmt, ...);

static FeatureFlags g_flags = {true, true, true, true, false, true, false};

const FeatureFlags& Features() { return g_flags; }

void Features_Load(const char* gameDir) {
    char path[MAX_PATH];
    sprintf_s(path, "%s\\kvr\\kvr.ini", gameDir);
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
    g_flags.cues = GetPrivateProfileIntA("Features", "Cues", 1, path) != 0;
    g_flags.weaponGallery = GetPrivateProfileIntA("Debug", "WeaponGallery", 0, path) != 0;
    g_flags.tracePosition = GetPrivateProfileIntA("Debug", "TracePosition", 0, path) != 0;
    Log("features: engineCamera %d weaponProxies %d gestures %d combat %d (%s)", g_flags.engineCamera,
        g_flags.weaponProxies, g_flags.gestures, g_flags.combat, path);
}
