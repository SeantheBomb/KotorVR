// KotOR VR launcher (the "KotOR VR" shortcut): starts the VR host next to it unless it is already running, then
// KotOR through Steam. Build: host\launch_build.bat.
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

// a live process of that name (a single-thread leftover from a lost VR runtime session doesn't count); with
// `kill`, it is ended instead (and reported as not running)
static bool Running(const wchar_t* exe, bool kill = false) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{sizeof e};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        if (_wcsicmp(e.szExeFile, exe) != 0 || e.cntThreads <= 1) continue;
        if (kill) {
            if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, e.th32ProcessID)) {
                TerminateProcess(h, 0);
                WaitForSingleObject(h, 3000);
                CloseHandle(h);
            }
            continue;
        }
        found = true;
    }
    CloseHandle(snap);
    return found;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (wchar_t* s = wcsrchr(dir, L'\\')) *s = 0;
    // a host left over without the game (an earlier session, a dropped connection) may be stuck: start a fresh one.
    // With the game running, the host belongs to it - leave it alone.
    if (!Running(L"swkotor.exe")) Running(L"kvr_host.exe", true);
    if (!Running(L"kvr_host.exe")) {
        wchar_t host[MAX_PATH];
        wsprintfW(host, L"%s\\kvr_host.exe", dir);
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(host, nullptr, nullptr, nullptr, FALSE, 0, nullptr, dir, &si, &pi)) {
            MessageBoxW(nullptr,
                        L"Could not start the KotOR VR host (kvr_host.exe).\n\nReinstall KotOR VR, or start kvr_host.exe "
                        L"from the kvr\\host folder of your KotOR install.",
                        L"KotOR VR", MB_ICONERROR);
            return 1;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Sleep(2500);  // let it open the VR session before the game starts sending frames
    }
    ShellExecuteW(nullptr, L"open", L"steam://rungameid/32370", nullptr, nullptr, SW_SHOWNORMAL);
    return 0;
}
