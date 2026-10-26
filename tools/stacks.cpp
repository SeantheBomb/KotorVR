// Sample every thread of a 32-bit process: EIP and an EBP-chain walk, as module+offset. Diagnoses hangs without a
// debugger. Build x86: cl /nologo /O2 stacks.cpp psapi.lib   usage: stacks <pid> [samples]
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <vector>

struct Mod { DWORD base, end; char name[64]; };
static std::vector<Mod> g_mods;

static void LoadMods(HANDLE p) {
    HMODULE mods[512];
    DWORD need;
    if (!EnumProcessModules(p, mods, sizeof mods, &need)) return;
    for (DWORD i = 0; i < need / sizeof(HMODULE); i++) {
        MODULEINFO mi;
        Mod m;
        GetModuleInformation(p, mods[i], &mi, sizeof mi);
        GetModuleBaseNameA(p, mods[i], m.name, sizeof m.name);
        m.base = (DWORD)(ULONG_PTR)mi.lpBaseOfDll;
        m.end = m.base + mi.SizeOfImage;
        g_mods.push_back(m);
    }
}

static void Print(DWORD a) {
    for (auto& m : g_mods)
        if (a >= m.base && a < m.end) {
            printf(" %s+%lX", m.name, a - m.base);
            return;
        }
    printf(" %08lX", a);
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    DWORD pid = atoi(argv[1]);
    int samples = argc > 2 ? atoi(argv[2]) : 3;
    HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!p) { printf("open process failed %lu\n", GetLastError()); return 1; }
    LoadMods(p);
    for (int s = 0; s < samples; s++) {
        printf("=== sample %d\n", s);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te = {sizeof te};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!t) continue;
            FILETIME c, e, k, u;
            GetThreadTimes(t, &c, &e, &k, &u);
            SuspendThread(t);
            CONTEXT ctx = {};
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(t, &ctx)) {
                printf("tid %5lu user %6llu ms:", te.th32ThreadID,
                       ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime) / 10000);
                Print(ctx.Eip);
                DWORD ebp = ctx.Ebp;
                for (int f = 0; f < 14 && ebp; f++) {
                    DWORD fr[2];
                    SIZE_T got;
                    if (!ReadProcessMemory(p, (void*)(ULONG_PTR)ebp, fr, 8, &got) || got != 8) break;
                    printf(" <");
                    Print(fr[1]);
                    if (fr[0] <= ebp) break;
                    ebp = fr[0];
                }
                // raw stack scan for return addresses inside swkotor.exe (frames without EBP)
                DWORD stk[256];
                SIZE_T got;
                if (ReadProcessMemory(p, (void*)(ULONG_PTR)ctx.Esp, stk, sizeof stk, &got)) {
                    printf("\n      exe refs:");
                    int shown = 0;
                    for (int i = 0; i < (int)(got / 4) && shown < 12; i++)
                        if (stk[i] >= 0x401000 && stk[i] < 0x760000) { printf(" %08lX", stk[i]); shown++; }
                }
                printf("\n");
            }
            ResumeThread(t);
            CloseHandle(t);
        }
        CloseHandle(snap);
        Sleep(300);
    }
    return 0;
}
