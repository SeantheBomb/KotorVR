// KotOR VR launcher (the "KotOR VR" shortcut): starts the VR host next to it unless it is already running, then
// KotOR through Steam. Build: host\launch_build.bat.
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

static bool HostRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{sizeof e};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e)) {
        if (_wcsicmp(e.szExeFile, L"kvr_host.exe") != 0) continue;
        // a single-thread leftover from a lost VR runtime session doesn't count
        found = e.cntThreads > 1;
    }
    CloseHandle(snap);
    return found;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (wchar_t* s = wcsrchr(dir, L'\\')) *s = 0;
    if (!HostRunning()) {
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
