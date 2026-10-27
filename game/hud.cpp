// The flat HUD's layout, published for the host's wrist panel (see hud.h and docs/DIEGETIC_HUD.md).
#include "hud.h"

#include <windows.h>

#include <cstring>
#include <initializer_list>

#include "../common/kvr_shared.h"
#include "dialogue.h"
#include "gui.h"
#include "vr.h"

extern void Log(const char* fmt, ...);

namespace {

BYTE* MainInterface() {
    __try {
        BYTE* app = *(BYTE**)0x7A39FC;
        BYTE* client = app ? *(BYTE**)(app + 4) : nullptr;
        BYTE* inner = client ? *(BYTE**)(client + 4) : nullptr;
        BYTE* gui = inner ? *(BYTE**)(inner + 0x40) : nullptr;
        return gui ? *(BYTE**)(gui + 0x90) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

struct Rect {
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    void Add(const int* e) {  // a control extent: x, y, w, h
        if (e[2] <= 0 || e[3] <= 0 || e[2] > 4096 || e[3] > 4096) return;
        if (e[0] < x0) x0 = e[0];
        if (e[1] < y0) y0 = e[1];
        if (e[0] + e[2] > x1) x1 = e[0] + e[2];
        if (e[1] + e[3] > y1) y1 = e[1] + e[3];
    }
    bool Valid() const { return x1 > x0 && y1 > y0; }
};

const int* Extent(BYTE* mi, int controlOffset) { return (const int*)(mi + controlOffset + 4); }

bool ReadLayout(BYTE* mi, Rect out[KVR_HUD_REGIONS]) {
    __try {
        static const int menu[] = {0xB00C, 0xB1D0, 0xB394, 0xB558, 0xB71C, 0xB8E0, 0xBAA4, 0xBC68};
        for (int o : menu) out[KVR_HUD_MENU].Add(Extent(mi, o));
        out[KVR_HUD_MINIMAP].Add(Extent(mi, 0x5CC0));
        for (int i = 0; i < 3; i++) {
            BYTE* w = mi + 0x1F88 + i * 0xEA8;
            for (int o : {0x28, 0x668, 0x7A8, 0x8F8, 0xCC8}) out[KVR_HUD_PARTY].Add(Extent(w, o));
        }
        for (int i = 0; i < 4; i++) {  // the four visible personal slots (6 exist)
            BYTE* s = mi + 0x772C + i * 0x71C;
            for (int o : {0x0, 0x388, 0x54C}) out[KVR_HUD_SLOTS].Add(Extent(s, o));
        }
        for (int o : {0xC0AC, 0xC360, 0xC614}) out[KVR_HUD_TOGGLES].Add(Extent(mi, o));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

void LogRaw(BYTE* mi) {  // once: the raw extents, to check them against a GUI frame
    __try {
        auto L = [&](const char* name, const int* e) { Log("hud: %-12s %5d %5d %5d %5d", name, e[0], e[1], e[2], e[3]); };
        L("BTN_EQU", Extent(mi, 0xB00C));
        L("BTN_OPT", Extent(mi, 0xBC68));
        L("MAPBORDER", Extent(mi, 0x5CC0));
        L("LBL_MAP", Extent(mi, 0x5E00));
        L("party0 BACK", Extent(mi + 0x1F88, 0x28));
        L("party0 CHAR", Extent(mi + 0x1F88, 0x668));
        L("party0 VIT", Extent(mi + 0x1F88, 0x7A8));
        L("party1 CHAR", Extent(mi + 0x1F88 + 0xEA8, 0x668));
        L("slot0 BTN", Extent(mi + 0x772C, 0x0));
        L("slot0 UP", Extent(mi + 0x772C, 0x388));
        L("slot3 BTN", Extent(mi + 0x772C + 3 * 0x71C, 0x0));
        L("TB_PAUSE", Extent(mi, 0xC0AC));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

}  // namespace

void Hud_Update() {
    KvrShared* shm = const_cast<KvrShared*>(VR_Shared());
    if (!shm) return;
    GuiState gs = Gui_State();
    shm->menuOpen = gs.menuPanels > 0;
    BYTE* mi = MainInterface();
    if (!mi) return;
    static bool logged;
    if (!logged) {
        logged = true;
        LogRaw(mi);
    }
    static DWORD last;
    if (GetTickCount() - last < 500) return;  // the layout only changes with the resolution
    last = GetTickCount();
    Rect r[KVR_HUD_REGIONS];
    if (!ReadLayout(mi, r)) return;
    bool changed = false;
    for (int i = 0; i < KVR_HUD_REGIONS; i++) {
        int v[4] = {r[i].Valid() ? r[i].x0 : 0, r[i].Valid() ? r[i].y0 : 0, r[i].Valid() ? r[i].x1 - r[i].x0 : 0,
                    r[i].Valid() ? r[i].y1 - r[i].y0 : 0};
        if (memcmp(v, shm->hud.rect[i], sizeof v)) {
            memcpy(shm->hud.rect[i], v, sizeof v);
            changed = true;
        }
    }
    if (changed) {
        MemoryBarrier();
        shm->hud.serial = shm->hud.serial + 1;
        Log("hud: layout menu %d,%d %dx%d | map %d,%d %dx%d | party %d,%d %dx%d | slots %d,%d %dx%d", shm->hud.rect[0][0],
            shm->hud.rect[0][1], shm->hud.rect[0][2], shm->hud.rect[0][3], shm->hud.rect[1][0], shm->hud.rect[1][1],
            shm->hud.rect[1][2], shm->hud.rect[1][3], shm->hud.rect[2][0], shm->hud.rect[2][1], shm->hud.rect[2][2],
            shm->hud.rect[2][3], shm->hud.rect[3][0], shm->hud.rect[3][1], shm->hud.rect[3][2], shm->hud.rect[3][3]);
    }
}
