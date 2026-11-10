// KotOR's minigames, read from the client area.
//
// client = [[0x7A39FC]+4]; its module = CClientExoApp 0x5ED530 (thiscall, -> [client+4] -> 0x644760); the module's
// area at +0x48 (CSWCArea); the area's minigame at +0x264 (null outside one: the area loader 0x6087E0 loads it from the
// .are's MiniGame struct, 0x6723D0); its Type (1 swoop race, 2 turret) at +0x80. The same chain is the game's own
// (0x60AA49, 0x647150). Only four areas have one (scan of every module's .are, 2026-11-10): m03mg, m17mg, m26mg
// (swoops) and m12ab (the Ebon Hawk's turrets).
#include "minigame.h"

#include <windows.h>

extern void Log(const char* fmt, ...);

namespace {

int ReadType() {
    typedef BYTE*(__fastcall * GetModuleFn)(void* client, void*);
    __try {
        BYTE* app = *(BYTE**)0x7A39FC;
        void* client = app ? *(void**)(app + 4) : nullptr;
        if (!client) return MG_NONE;
        BYTE* module = ((GetModuleFn)0x5ED530)(client, nullptr);
        BYTE* area = module ? *(BYTE**)(module + 0x48) : nullptr;
        BYTE* mg = area ? *(BYTE**)(area + 0x264) : nullptr;
        if (!mg) return MG_NONE;
        int t = *(int*)(mg + 0x80);
        return t == MG_SWOOP || t == MG_TURRET ? t : MG_NONE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return MG_NONE;
}

}  // namespace

int Minigame_Type() {
    static int type = MG_NONE;
    static DWORD last;
    DWORD t = GetTickCount();
    if (t - last < 250) return type;
    last = t;
    int now = ReadType();
    if (now != type) {
        Log("minigame: %s", now == MG_SWOOP ? "swoop race - riding the game's chase camera"
                            : now == MG_TURRET ? "turret - the game's gunner camera"
                                               : "over - back in first person");
        type = now;
    }
    return type;
}
