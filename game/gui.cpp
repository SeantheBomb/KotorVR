// KotOR's GUI manager state (names from Lane's K1 Ghidra labels; docs/FINDINGS.md "GUI manager").
//
// CSWGuiManager* = [0x7A39F4]: the shown panels (CExoArrayList<CSWGuiPanel*>) at +0x88 / count +0x8C, the modal stack
// at +0x94 / count +0x98. Measured (2026-10-27): in the world the list holds the main interface (HUD) and one
// persistent companion panel; a game screen adds its own panel - the computer terminal (the HUD leaves), the
// inventory (with the in-game menu frame), and by the same mechanism containers, stores, the workbench... NPC
// conversations and cutscenes add the dialog panels, which our own conversation panel replaces.
// So "a game menu is up" = any shown panel other than the HUD, its companion and the dialog / cinematic panels.
#include "gui.h"

#include <windows.h>

#include <cstdio>

extern void Log(const char* fmt, ...);

namespace {

// Panels identified by their first virtual function (stable across instances).
const DWORD kHudFns[] = {0x68D3B0, 0x624900};  // main interface, its persistent companion
// Dialog family vtables (constructors 0x6A8B40 cinematic, 0x6A8930 letterbox, 0x6A78C0 top bar, 0x6A85B0 base).
const DWORD kDialogVtbls[] = {0x755800, 0x755A78, 0x755790, 0x7559E0};

bool IsDialogPanel(void* p) {
    DWORD vt = *(DWORD*)p;
    for (DWORD v : kDialogVtbls)
        if (vt == v) return true;
    return false;
}

bool IsWorldPanel(void* p) {
    DWORD fn = **(DWORD**)p;
    for (DWORD f : kHudFns)
        if (fn == f) return true;
    return IsDialogPanel(p);
}

bool Read(GuiState* s, DWORD* sig) {
    __try {
        BYTE* mgr = *(BYTE**)0x7A39F4;
        if (!mgr) return false;
        int n = *(int*)(mgr + 0x8C);
        void** data = *(void***)(mgr + 0x88);
        int m = *(int*)(mgr + 0x98);
        if (n < 0 || n > 64 || !data || m < 0 || m > 64) return false;
        s->modalCount = m;
        *sig = n * 7 + m;
        for (int i = 0; i < n; i++) {
            if (!data[i]) continue;
            *sig = *sig * 31 + *(DWORD*)data[i];
            if (IsDialogPanel(data[i])) s->dialogPanels++;
            if (!IsWorldPanel(data[i])) {
                s->menuPanels++;
                s->menuVtbl = *(DWORD*)data[i];
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

void LogList() {
    __try {
        BYTE* mgr = *(BYTE**)0x7A39F4;
        int n = *(int*)(mgr + 0x8C);
        void** data = *(void***)(mgr + 0x88);
        char line[1024];
        int len = sprintf_s(line, "gui: %d panels (vtable/first fn):", n);
        for (int i = 0; i < n && len < 960; i++)
            if (data[i])
                len += sprintf_s(line + len, sizeof line - len, " %08lX/%08lX", *(DWORD*)data[i], **(DWORD**)data[i]);
        Log("%s, modal %d", line, *(int*)(mgr + 0x98));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

}  // namespace

GuiState Gui_State() {
    GuiState s{};
    DWORD sig = 0;
    if (!Read(&s, &sig)) return s;
    static DWORD lastSig;
    if (sig != lastSig) {
        lastSig = sig;
        LogList();
        if (s.menuPanels) Log("gui: game menu up (panel vtable %08lX)", s.menuVtbl);
    }
    return s;
}
