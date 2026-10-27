// Compatibility with programs that inject into games (see compat.h).
//
// Discord's in-game overlay (DiscordHook.dll) injects itself into KotOR once the game is running and hooks the same
// OpenGL calls we do; with it the game froze in the main menu (2026-10-27, harness run with Discord open: the log
// ends on the overlay looking up its GLSL and NV_DX_interop entry points). Its picture only lands on the flat window
// anyway, never in the headset. So, unless kvr.ini [Compat] DiscordOverlay=1, the overlay's DLL is refused when it
// tries to load into this process. Discord itself (voice, chat, streaming) is untouched.
#include "compat.h"

#include <windows.h>
#include <winternl.h>

#include <cwchar>

extern void Log(const char* fmt, ...);

namespace {

typedef LONG(NTAPI* LdrLoadDllFn)(PWSTR path, PULONG flags, PUNICODE_STRING name, PVOID* base);
BYTE* g_tramp;
volatile LONG g_refused;

bool Blocked(const UNICODE_STRING* name) {
    if (!name || !name->Buffer) return false;
    // the last path component, case-insensitively
    int n = name->Length / 2;
    const wchar_t* s = name->Buffer;
    int start = 0;
    for (int i = 0; i < n; i++)
        if (s[i] == L'\\' || s[i] == L'/') start = i + 1;
    static const wchar_t* kBlocked[] = {L"discordhook"};
    for (const wchar_t* b : kBlocked) {
        size_t bl = wcslen(b);
        if ((size_t)(n - start) >= bl && _wcsnicmp(s + start, b, bl) == 0) return true;
    }
    return false;
}

LONG NTAPI h_LdrLoadDll(PWSTR path, PULONG flags, PUNICODE_STRING name, PVOID* base) {
    if (Blocked(name)) {
        if (InterlockedIncrement(&g_refused) <= 3)
            Log("compat: refused %.*ls in KotOR (Discord's overlay; [Compat] DiscordOverlay=1 allows it)",
                name->Length / 2, name->Buffer);
        if (base) *base = nullptr;
        return (LONG)0xC0000135;  // STATUS_DLL_NOT_FOUND
    }
    return ((LdrLoadDllFn)g_tramp)(path, flags, name, base);
}

}  // namespace

void Compat_Install(bool allowDiscordOverlay) {
    if (allowDiscordOverlay) {
        Log("compat: Discord overlay allowed ([Compat] DiscordOverlay=1)");
        return;
    }
    if (GetModuleHandleA("DiscordHook.dll")) {
        Log("compat: Discord's overlay was already loaded before us - can't keep it out");
        return;
    }
    BYTE* target = (BYTE*)GetProcAddress(GetModuleHandleA("ntdll.dll"), "LdrLoadDll");
    static const BYTE kPrologue[5] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};  // mov edi, edi; push ebp; mov ebp, esp
    if (!target) return;
    g_tramp = (BYTE*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (target[0] == 0xE9) {
        // already hooked by someone else (Steam's overlay does this): go in front of them - our trampoline jumps
        // where their jump went, so both keep working
        BYTE* dest = target + 5 + *(INT32*)(target + 1);
        g_tramp[0] = 0xE9;
        *(INT32*)(g_tramp + 1) = (INT32)(dest - (g_tramp + 5));
        Log("compat: LdrLoadDll already hooked (-> %p): chaining in front", dest);
    } else if (memcmp(target, kPrologue, 5) == 0) {
        memcpy(g_tramp, target, 5);
        g_tramp[5] = 0xE9;
        *(INT32*)(g_tramp + 6) = (INT32)((target + 5) - (g_tramp + 10));
    } else {
        Log("compat: LdrLoadDll prologue %02X %02X %02X %02X %02X not as expected - overlay blocking off", target[0],
            target[1], target[2], target[3], target[4]);
        VirtualFree(g_tramp, 0, MEM_RELEASE);
        g_tramp = nullptr;
        return;
    }
    DWORD old;
    VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xE9;
    *(INT32*)(target + 1) = (INT32)((BYTE*)h_LdrLoadDll - (target + 5));
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    Log("compat: Discord's overlay will be kept out of KotOR");
}
