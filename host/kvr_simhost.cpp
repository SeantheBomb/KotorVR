// kvr_simhost: headset-free stand-in for kvr_host, for automated tests.
//
// Speaks the same shared-memory contract as kvr_host (common/kvr_shared.h) but without OpenXR: it plays a
// scripted timeline of head poses and controller input into shared memory, consumes the game's eye/panel
// textures with the same keyed-mutex handoff (so the game keeps delivering frames), and saves PNG screenshots
// and a stats CSV. The game layer cannot tell it apart from the real host.
//
// usage: kvr_simhost.exe <script.txt> <out_dir>
//
// Script: one command per line, '#' comments. Durations in seconds, angles in degrees.
//   wait <s>                          idle
//   wait_menu <timeout>               until the game exports frames (main menu or later)
//   wait_world <timeout>              until the 3D world renders in VR (eyes delivered, world frame recent)
//   head yaw=<d> pitch=<d> [x= y= z=] [over=<s>]   move the head (relative to the recenter origin), lerped
//   stick [lx=] [ly=] [rx=] [ry=] [for=<s>]        hold thumbsticks, then release (omit 'for' to keep holding)
//   button <A|B|X|Y|MENU|LSTICK|RSTICK> [for=<s>]  press and release a button (default 0.15 s)
//   click u=<0..1> v=<0..1> [hold=<s>]             point at the panel (top-left origin) and pull the trigger
//   grip [for=<s>]                    squeeze the right grip (default action) and release
//   squeeze <L|R> [grip=<0..1>] [trig=<0..1>]   hold analog grip / trigger on a hand (finger curl)
//   hand <L|R> [x= y= z=] [yaw= pitch= roll=]      place a hand (grip pose, relative to the recenter origin, deg)
//   swing [dx= dy= dz=] [over=<s>]    sweep the right hand by (dx,dy,dz) m over <s> (default 0.6,0.3,0 in 0.18 s)
//   trigger [for=<s>]                 pull the right trigger without pointing at the panel
//   key <name>                        tap a key in the game window: grave (console), enter, escape, space, f1..f12
//   type <text...>                    type text into the game window (the rest of the line, spaces included)
//   wait_log <substring> <timeout>    until <game>\kvr\probe.log contains the substring (e.g. "combat round active")
//   wait_new_log <substring> <timeout>  same, only lines written after the command starts; a sync point (never fails)
//   wait_calm <seconds> <timeout>     until the panel has delivered frames without a gap for <seconds> (before menu clicks)
//   pointer none|world                stop pointing / point into the world along the head direction
//   shot <name>                       save <name>_eye0.png, <name>_eye1.png, <name>_panel.png
//   expect_world                      fail the run unless the world is rendering
//   game <text...>                    a debug command for the game DLL (<game>\kvr\simcmd.txt; see game/simcmd.cpp):
//                                     goto x y [x y ...], face x y, pos, stop, ...
//   check_log <name> <substring> <timeout>          CHECK: the substring appears in the log within <timeout> s
//   check_no_log <name> <substring> <seconds>       CHECK: the substring does not appear for <seconds>
//   check_count <name> <substring> <min> <max> <seconds>  CHECK: it appears min..max times in the next <seconds>
//   hand_adjust                       turn the right hand by the game's last "aimhint" (after: game aimhint)
//   mark / check_since_mark <name> <substring> <timeout>  like check_log, from the last mark (events logged early)
//                                     '~' in a substring stands for a space. Checks print "CHECK PASS/FAIL <name>"
//                                     and the run goes on; any FAIL makes the exit code 1.
//   end                               stop (implicit at end of file)
// Exit code 0 = script completed, 1 = a wait timed out or an expectation failed, 2 = setup error.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <tlhelp32.h>
#include <string>
#include <vector>

#include "../common/kvr_shared.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

enum { KEY_GAME = 0, KEY_HOST = 1 };  // must match game/vr.cpp and host/kvr_host.cpp

static const double kPi = 3.14159265358979;
static KvrShared* g_shm;
static ID3D11Device1* g_dev;
static ID3D11DeviceContext* g_ctx;
static FILE* g_csv;
static std::string g_out;
static LARGE_INTEGER g_qpf, g_t0;
static volatile bool g_quit;

static double Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart - g_t0.QuadPart) / g_qpf.QuadPart;
}

// ---------------------------------------------------------------- shared memory
static bool OpenShared() {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(KvrShared), KVR_SHM_NAME);
    if (!h) return false;
    bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    g_shm = (KvrShared*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(KvrShared));
    if (!g_shm) return false;
    if (!existed || g_shm->magic != KVR_MAGIC || g_shm->version != KVR_VERSION) {
        ZeroMemory(g_shm, sizeof(KvrShared));
        g_shm->magic = KVR_MAGIC;
        g_shm->version = KVR_VERSION;
    }
    return true;
}

// ---------------------------------------------------------------- simulated tracking
struct Head {
    double yaw = 0, pitch = 0;  // degrees; yaw positive = turn left (counter-clockwise from above), like OpenXR
    double x = 0, y = 0, z = 0;
};
static Head g_head, g_headFrom, g_headTo;
static double g_headT0, g_headDur;

static bool g_aimHand;  // aimhand: the pointer follows the right hand
static void QuatYawPitch(double yawDeg, double pitchDeg, float q[4]) {
    double y = yawDeg * kPi / 360.0, p = pitchDeg * kPi / 360.0;  // half angles
    // q = yaw(about +Y) * pitch(about +X)
    double cy = cos(y), sy = sin(y), cp = cos(p), sp = sin(p);
    q[0] = (float)(cy * sp);
    q[1] = (float)(sy * cp);
    q[2] = (float)(-sy * sp);
    q[3] = (float)(cy * cp);
}

static void Rotate(const float q[4], const float v[3], float out[3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

static void PublishHead() {
    double t = g_headDur > 0 ? std::min(1.0, (Now() - g_headT0) / g_headDur) : 1.0;
    auto L = [&](double a, double b) { return a + (b - a) * t; };
    g_head.yaw = L(g_headFrom.yaw, g_headTo.yaw);
    g_head.pitch = L(g_headFrom.pitch, g_headTo.pitch);
    g_head.x = L(g_headFrom.x, g_headTo.x);
    g_head.y = L(g_headFrom.y, g_headTo.y);
    g_head.z = L(g_headFrom.z, g_headTo.z);

    float q[4];
    QuatYawPitch(g_head.yaw, g_head.pitch, q);
    KvrHostState& h = g_shm->host;
    h.seq++;
    MemoryBarrier();
    for (int e = 0; e < 2; e++) {
        float off[3] = {e == 0 ? -0.032f : 0.032f, 0, 0}, w[3];  // 64 mm IPD
        Rotate(q, off, w);
        h.eye[e].pos[0] = (float)g_head.x + w[0];
        h.eye[e].pos[1] = (float)g_head.y + w[1];
        h.eye[e].pos[2] = (float)g_head.z + w[2];
        memcpy(h.eye[e].quat, q, sizeof q);
        // Quest 3-like per-eye FOV (radians), slightly canted outward
        h.fov[e] = e == 0 ? KvrFov{-0.942f, 0.698f, 0.768f, -0.890f} : KvrFov{-0.698f, 0.942f, 0.768f, -0.890f};
    }
    h.recommendedW = 1824;
    h.recommendedH = 1968;
    h.running = 1;
    MemoryBarrier();
    h.seq++;
}

// ---------------------------------------------------------------- simulated controllers
struct Controls {
    float lx = 0, ly = 0, rx = 0, ry = 0, trigR = 0, gripR = 0, trigL = 0, gripL = 0;
    uint32_t buttons = 0;
    uint32_t pointer = KVR_POINTER_NONE;
    float u = 0, v = 0;
};
static Controls g_ctl;

// ---------------------------------------------------------------- keyboard into the game (console commands)
static HWND GameWindow() {
    // the visible top-level window of swkotor.exe (titles are not unique enough)
    static DWORD pid;
    static double pidAt = -10;
    if (Now() - pidAt > 5) {
        pidAt = Now();
        pid = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32 pe = {sizeof pe};
        for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe))
            if (!_stricmp(pe.szExeFile, "swkotor.exe")) pid = pe.th32ProcessID;
        CloseHandle(snap);
    }
    if (!pid) return nullptr;
    struct Find { DWORD pid; HWND w; } f = {pid, nullptr};
    EnumWindows([](HWND w, LPARAM p) -> BOOL {
        Find* f = (Find*)p;
        DWORD wp = 0;
        GetWindowThreadProcessId(w, &wp);
        if (wp == f->pid && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) { f->w = w; return FALSE; }
        return TRUE;
    }, (LPARAM)&f);
    return f.w;
}

static void FocusGame() {
    HWND w = GameWindow();
    if (!w || GetForegroundWindow() == w) return;
    printf("[%.1f]   (refocusing the game; foreground was %p)\n", Now(), (void*)GetForegroundWindow());
    INPUT alt[2] = {};
    alt[0].type = alt[1].type = INPUT_KEYBOARD;
    alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, alt, sizeof(INPUT));
    SetForegroundWindow(w);
    Sleep(150);
}

static void TapKey(WORD vk) {
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = vk;
    in[0].ki.wScan = in[1].ki.wScan = (WORD)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

static void TypeChar(char c) {
    SHORT v = VkKeyScanA(c);
    if (v == -1) return;
    bool shift = (v >> 8) & 1;
    INPUT in[4] = {};
    int n = 0;
    auto key = [&](WORD vk, bool up) {
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wVk = vk;
        in[n].ki.wScan = (WORD)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
        in[n].ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        n++;
    };
    if (shift) key(VK_SHIFT, false);
    key((WORD)(v & 0xFF), false);
    key((WORD)(v & 0xFF), true);
    if (shift) key(VK_SHIFT, true);
    SendInput(n, in, sizeof(INPUT));
}

static const char* kProbeLog = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\swkotor\\kvr\\probe.log";

static long LogSize() {
    FILE* f = _fsopen(kProbeLog, "rb", _SH_DENYNO);
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

// `from`: byte offset to start at (wait_new_log only looks at lines written after the command started)
static int LogCount(const std::string& needle, long from) {
    FILE* f = _fsopen(kProbeLog, "rb", _SH_DENYNO);
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    if (end <= from) {
        fclose(f);
        return 0;
    }
    std::string all(end - from, '\0');
    fseek(f, from, SEEK_SET);
    size_t n = fread(&all[0], 1, all.size(), f);
    fclose(f);
    all.resize(n);
    int c = 0;
    for (size_t q = all.find(needle); q != std::string::npos; q = all.find(needle, q + needle.size())) c++;
    return c;
}

static std::string GameCmdPath() {
    std::string q = kProbeLog;
    return q.substr(0, q.rfind('\\')) + "\\simcmd.txt";
}

static bool LogContains(const std::string& needle, long from = 0) {
    FILE* f = _fsopen(kProbeLog, "rb", _SH_DENYNO);
    if (!f) return false;
    fseek(f, from, SEEK_SET);
    static std::string buf;
    buf.assign(1 << 20, '\0');
    bool hit = false;
    size_t n;
    std::string carry;
    while ((n = fread(&buf[0], 1, buf.size(), f)) > 0) {
        std::string chunk = carry + buf.substr(0, n);
        if (chunk.find(needle) != std::string::npos) { hit = true; break; }
        carry = chunk.size() > needle.size() ? chunk.substr(chunk.size() - needle.size()) : chunk;
    }
    fclose(f);
    return hit;
}

// grip poses: resting low in front of the body, weapons pointing forward and slightly up
struct HandPose {
    double x, y, z, yaw, pitch, roll;
};
static HandPose g_hand[2] = {{-0.22, -0.40, -0.35, 0, 20, 0}, {0.22, -0.40, -0.35, 0, 20, 0}};
static HandPose g_swingFrom, g_swingTo;
static bool g_checkFailed;  // a check_* command failed (the run continues, the exit code says so)
static long g_mark;         // log offset of the last "mark" command
static double g_swingT0, g_swingDur;  // right-hand sweep in progress when g_swingDur > 0
struct Release {
    double at;
    int what;  // 0 sticks, 1 button mask, 2 trigger, 3 grip
    uint32_t mask;
};
static std::vector<Release> g_releases;

static void PublishInput() {
    double now = Now();
    for (size_t i = 0; i < g_releases.size();) {
        const Release& r = g_releases[i];
        if (now >= r.at) {
            if (r.what == 0) g_ctl.lx = g_ctl.ly = g_ctl.rx = g_ctl.ry = 0;
            if (r.what == 1) g_ctl.buttons &= ~r.mask;
            if (r.what == 2) g_ctl.trigR = 0;
            if (r.what == 3) g_ctl.gripR = 0;
            g_releases.erase(g_releases.begin() + i);
        } else {
            i++;
        }
    }
    KvrInput& in = g_shm->input;
    in.seq++;
    MemoryBarrier();
    in.active = 1;
    in.lx = g_ctl.lx; in.ly = g_ctl.ly; in.rx = g_ctl.rx; in.ry = g_ctl.ry;
    in.trigL = g_ctl.trigL; in.trigR = g_ctl.trigR; in.gripL = g_ctl.gripL; in.gripR = g_ctl.gripR;
    in.buttons = g_ctl.buttons;
    in.pointerMode = g_ctl.pointer;
    in.panelU = g_ctl.u;
    in.panelV = g_ctl.v;
    if (g_aimHand) {  // "aimhand": the pointer is the right hand's ray (as on a real controller)
        QuatYawPitch(g_hand[1].yaw, g_hand[1].pitch, in.aim.quat);
        in.aim.pos[0] = (float)g_hand[1].x; in.aim.pos[1] = (float)g_hand[1].y; in.aim.pos[2] = (float)g_hand[1].z;
    } else {
        QuatYawPitch(g_head.yaw, g_head.pitch, in.aim.quat);  // world pointer follows the head if ever used
        in.aim.pos[0] = (float)g_head.x; in.aim.pos[1] = (float)g_head.y - 0.3f; in.aim.pos[2] = (float)g_head.z - 0.2f;
    }
    if (g_swingDur > 0) {
        double t = std::min(1.0, (now - g_swingT0) / g_swingDur);
        g_hand[1].x = g_swingFrom.x + (g_swingTo.x - g_swingFrom.x) * t;
        g_hand[1].y = g_swingFrom.y + (g_swingTo.y - g_swingFrom.y) * t;
        g_hand[1].z = g_swingFrom.z + (g_swingTo.z - g_swingFrom.z) * t;
        if (t >= 1.0) g_swingDur = 0;
    }
    for (int h = 0; h < 2; h++) {
        const HandPose& hp = g_hand[h];
        float q[4];
        QuatYawPitch(hp.yaw, hp.pitch, q);
        if (hp.roll != 0) {  // then roll about the controller's own forward axis (-Z): q * r(+Z, roll)
            double r = hp.roll * kPi / 360.0;
            float rq[4] = {0, 0, (float)sin(r), (float)cos(r)}, o[4];
            o[0] = q[3] * rq[0] + q[0] * rq[3] + q[1] * rq[2] - q[2] * rq[1];
            o[1] = q[3] * rq[1] - q[0] * rq[2] + q[1] * rq[3] + q[2] * rq[0];
            o[2] = q[3] * rq[2] + q[0] * rq[1] - q[1] * rq[0] + q[2] * rq[3];
            o[3] = q[3] * rq[3] - q[0] * rq[0] - q[1] * rq[1] - q[2] * rq[2];
            memcpy(q, o, sizeof o);
        }
        in.grip[h].pos[0] = (float)hp.x; in.grip[h].pos[1] = (float)hp.y; in.grip[h].pos[2] = (float)hp.z;
        memcpy(in.grip[h].quat, q, sizeof q);
    }
    in.gripValid = 3;
    in.time = now;
    MemoryBarrier();
    in.seq++;
}

// ---------------------------------------------------------------- frame consumption (same handoff as kvr_host)
struct SharedTex {
    const wchar_t* name;
    ID3D11Texture2D* tex = nullptr;
    IDXGIKeyedMutex* mutex = nullptr;
    ID3D11Texture2D* staging = nullptr;  // CPU-readable copy of the latest frame
    KvrEyeSlot meta{};
    bool have = false;
};
static SharedTex g_shared[3] = {{KVR_TEX_NAME_EYE0}, {KVR_TEX_NAME_EYE1}, {KVR_TEX_NAME_PANEL}};
static uint32_t g_sharedGen;
static uint32_t g_pulls[3];

static void ReleaseShared() {
    for (auto& s : g_shared) {
        if (s.mutex) s.mutex->Release();
        if (s.tex) s.tex->Release();
        if (s.staging) s.staging->Release();
        s.mutex = nullptr; s.tex = nullptr; s.staging = nullptr;
        s.have = false;
    }
    g_sharedGen = 0;
}

static bool EnsureShared() {
    if (!g_shm->texWidth) return false;
    if (g_sharedGen == g_shm->texGeneration && g_shared[0].tex) return true;
    ReleaseShared();
    for (auto& s : g_shared) {
        wchar_t name[96];
        swprintf_s(name, L"%s_%u", s.name, g_shm->texGeneration);
        if (FAILED(g_dev->OpenSharedResourceByName(name, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, IID_PPV_ARGS(&s.tex)))) {
            ReleaseShared();
            return false;
        }
        s.tex->QueryInterface(IID_PPV_ARGS(&s.mutex));
        D3D11_TEXTURE2D_DESC d;
        s.tex->GetDesc(&d);
        d.MiscFlags = 0;
        d.BindFlags = 0;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        g_dev->CreateTexture2D(&d, nullptr, &s.staging);
    }
    g_sharedGen = g_shm->texGeneration;
    printf("[%.1f] opened shared textures %ux%u (gen %u)\n", Now(), g_shm->texWidth, g_shm->texHeight, g_sharedGen);
    return true;
}

static void PullAll() {
    if (!EnsureShared()) return;
    const volatile KvrEyeSlot* slots[3] = {&g_shm->eyes[0], &g_shm->eyes[1], &g_shm->panel};
    for (int i = 0; i < 3; i++) {
        SharedTex& s = g_shared[i];
        if (!s.mutex || s.mutex->AcquireSync(KEY_HOST, 0) != S_OK) continue;
        g_ctx->CopyResource(s.staging, s.tex);
        s.meta = const_cast<const KvrEyeSlot&>(*slots[i]);
        s.have = true;
        g_pulls[i]++;
        s.mutex->ReleaseSync(KEY_GAME);
    }
}

static bool InWorld() {
    uint32_t alive = g_shm->gameAlive;
    return g_shared[0].have && g_shared[1].have && (alive - g_shm->lastWorldFrame) < 30;
}

// Write the staging texture as a top-down RGB PNG (GL rows are bottom-up).
static bool SavePng(SharedTex& s, const std::string& path) {
    if (!s.have || !s.staging) return false;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(s.staging, 0, D3D11_MAP_READ, 0, &m))) return false;
    UINT w = s.meta.width ? s.meta.width : g_shm->texWidth, h = s.meta.height ? s.meta.height : g_shm->texHeight;
    std::vector<BYTE> rgb(size_t(w) * h * 3);
    for (UINT y = 0; y < h; y++) {
        const BYTE* src = (const BYTE*)m.pData + size_t(h - 1 - y) * m.RowPitch;
        BYTE* dst = &rgb[size_t(y) * w * 3];
        for (UINT x = 0; x < w; x++) {  // RGBA -> BGR for WIC's 24bppBGR
            dst[x * 3 + 0] = src[x * 4 + 2];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 0];
        }
    }
    g_ctx->Unmap(s.staging, 0);

    IWICImagingFactory* f = nullptr;
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    bool ok = false;
    std::wstring wpath(path.begin(), path.end());
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h))) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        ok = SUCCEEDED(fr->SetPixelFormat(&fmt)) && SUCCEEDED(fr->WritePixels(h, w * 3, (UINT)rgb.size(), rgb.data())) &&
             SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    }
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    if (f) f->Release();
    return ok;
}

// Mean brightness (0..255) of the latest frame of a shared texture, sampled on an 8-pixel grid; -1 if none.
static double MeanLuma(SharedTex& s) {
    if (!s.have || !s.staging) return -1;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(s.staging, 0, D3D11_MAP_READ, 0, &m))) return -1;
    UINT w = s.meta.width ? s.meta.width : g_shm->texWidth, h = s.meta.height ? s.meta.height : g_shm->texHeight;
    double sum = 0;
    long n = 0;
    for (UINT y = 0; y < h; y += 8) {
        const BYTE* row = (const BYTE*)m.pData + size_t(y) * m.RowPitch;
        for (UINT x = 0; x < w; x += 8, n++) sum += 0.299 * row[x * 4] + 0.587 * row[x * 4 + 1] + 0.114 * row[x * 4 + 2];
    }
    g_ctx->Unmap(s.staging, 0);
    return n ? sum / n : -1;
}

// ---------------------------------------------------------------- main loop helpers
// the game's haptic requests, logged as "haptic L/R ..." (scripts check them with wait_out)
static void LogHaptics() {
    static uint32_t seen[2];
    static bool primed;
    for (int h = 0; h < 2; h++) {
        uint32_t serial = g_shm->haptic[h].serial;
        if (primed && serial != seen[h])
            printf("[%.1f]   haptic %c amp %.2f dur %.3f freq %.0f\n", Now(), h ? 'R' : 'L', g_shm->haptic[h].amplitude,
                   g_shm->haptic[h].duration, g_shm->haptic[h].frequency);
        seen[h] = serial;
    }
    static uint32_t flashSeen;
    if (primed && g_shm->flash.serial != flashSeen)
        printf("[%.1f]   flash rgb %.2f %.2f %.2f strength %.2f for %.2f s\n", Now(), g_shm->flash.rgb[0],
               g_shm->flash.rgb[1], g_shm->flash.rgb[2], g_shm->flash.strength, g_shm->flash.duration);
    flashSeen = g_shm->flash.serial;
    primed = true;
}

// Frame-flow tracking for wait_calm: KotOR can stall 10+ s right after the menu first shows; clicks then are lost.
static double g_lastPanelPull = -1, g_calmSince = -1;
static uint32_t g_seenPanelPulls;

static void TrackCalm() {
    double now = Now();
    if (g_pulls[2] != g_seenPanelPulls) {
        g_seenPanelPulls = g_pulls[2];
        if (g_lastPanelPull < 0 || now - g_lastPanelPull > 0.5) g_calmSince = now;  // frames resumed after a gap
        g_lastPanelPull = now;
    }
}

// the game's text panels, logged as "text <slot> ..." when they change (the simulator has no headset to draw them)
static void LogText() {
    static uint32_t seen[KVR_TEXT_PANELS];
    for (int i = 0; i < KVR_TEXT_PANELS; i++) {
        uint32_t s = g_shm->text[i].serial;
        if (s == seen[i]) continue;
        seen[i] = s;
        char first[96];
        strncpy_s(first, g_shm->text[i].text, _TRUNCATE);
        if (char* nl = strchr(first, '\n')) *nl = 0;
        printf("[%.1f]   text %d anchor %u width %.2f: %s\n", Now(), i, g_shm->text[i].anchor, g_shm->text[i].width,
               g_shm->text[i].anchor ? first : "(hidden)");
    }
}

// ---------------------------------------------------------------- recording ("record on" / "record off")
// The headset's left eye in the world, else the panel (menus, movies), 15 fps through ffmpeg into <out>\rec_N.mp4,
// for reviewing runs (pop-in) frame by frame. Frames are written as the GL images come (bottom-up), ffmpeg flips.
static HANDLE g_recPipe, g_recProc;
static double g_recLast;
static int g_recCount;
static std::vector<BYTE> g_recBuf;

static void RecordStart() {
    if (g_recProc || !g_shm->texWidth) return;
    char cmd[1024];
    sprintf_s(cmd, "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgba -s %ux%u -r 15 -i - -vf vflip,scale=960:-2 "
              "-c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p \"%s\\rec_%d.mp4\"",
              g_shm->texWidth, g_shm->texHeight, g_out.c_str(), ++g_recCount);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 32 << 20)) return;
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = rd;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        printf("[%.1f]   record: ffmpeg did not start\n", Now());
        CloseHandle(rd);
        CloseHandle(wr);
        return;
    }
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    g_recProc = pi.hProcess;
    g_recPipe = wr;
    g_recLast = 0;
    g_recBuf.assign((size_t)g_shm->texWidth * g_shm->texHeight * 4, 0);
    printf("[%.1f]   record: rec_%d.mp4\n", Now(), g_recCount);
}

static void RecordStop() {
    if (!g_recProc) return;
    CloseHandle(g_recPipe);
    WaitForSingleObject(g_recProc, 15000);
    CloseHandle(g_recProc);
    g_recProc = g_recPipe = nullptr;
}

static void RecordTick() {
    if (!g_recProc || Now() - g_recLast < 1.0 / 15) return;
    g_recLast = Now();
    SharedTex& s = InWorld() ? g_shared[0] : g_shared[2];
    if (!s.have || !s.staging) return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(s.staging, 0, D3D11_MAP_READ, 0, &m))) return;
    UINT W = g_shm->texWidth, H = g_shm->texHeight;
    UINT w = std::min(s.meta.width ? s.meta.width : W, W), h = std::min(s.meta.height ? s.meta.height : H, H);
    std::fill(g_recBuf.begin(), g_recBuf.end(), 0);
    for (UINT y = 0; y < h; y++) memcpy(&g_recBuf[(size_t)y * W * 4], (const BYTE*)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
    g_ctx->Unmap(s.staging, 0);
    DWORD n = 0;
    if (!WriteFile(g_recPipe, g_recBuf.data(), (DWORD)g_recBuf.size(), &n, nullptr)) RecordStop();
}

static void Tick() {
    PublishHead();
    PublishInput();
    PullAll();
    TrackCalm();
    LogHaptics();
    LogText();
    RecordTick();
    static double lastCsv;
    double now = Now();
    if (g_csv && now - lastCsv >= 0.1) {
        lastCsv = now;
        fprintf(g_csv, "%.2f,%u,%u,%u,%u,%d,%u,%.1f,%.1f\n", now, g_shm->gameAlive, g_shm->eyes[0].frameId, g_shm->eyes[1].frameId,
                g_shm->panel.frameId, InWorld() ? 1 : 0, g_shm->cinematic, g_head.yaw, g_head.pitch);
    }
    Sleep(13);  // ~72 Hz like the headset
}

// KotOR freezes when it loses the foreground. Once the game world is up (never in menus, where a refocus can
// eat clicks), take the foreground back every 2 s if something else grabbed it.
static bool g_keepFocus;
static bool g_keepFocusOff;  // "keepfocus off": behave like the real host (it never takes the foreground)
static void KeepFocus() {
    static double last;
    if (!g_keepFocus || Now() - last < 2.0) return;
    last = Now();
    FocusGame();
}

static void RunFor(double seconds) {
    double end = Now() + seconds;
    while (!g_quit && Now() < end) {
        Tick();
        KeepFocus();
    }
}

template <class F> static bool RunUntil(double timeout, F cond) {
    double end = Now() + timeout;
    while (!g_quit && Now() < end) {
        Tick();
        KeepFocus();
        if (cond()) return true;
    }
    return false;
}

// key=value argument parsing
static bool Arg(const std::vector<std::string>& a, const char* key, double& out) {
    std::string k = std::string(key) + "=";
    for (auto& s : a)
        if (s.compare(0, k.size(), k) == 0) { out = atof(s.c_str() + k.size()); return true; }
    return false;
}

static uint32_t ButtonMask(const std::string& n) {
    if (n == "A") return KVR_BTN_A;
    if (n == "B") return KVR_BTN_B;
    if (n == "X") return KVR_BTN_X;
    if (n == "Y") return KVR_BTN_Y;
    if (n == "MENU") return KVR_BTN_MENU;
    if (n == "LSTICK") return KVR_BTN_LSTICK;
    if (n == "RSTICK") return KVR_BTN_RSTICK;
    return 0;
}

static BOOL WINAPI OnConsoleClose(DWORD) {
    g_quit = true;
    if (g_shm) { g_shm->host.running = 0; g_shm->input.active = 0; }
    Sleep(100);
    return FALSE;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { printf("usage: kvr_simhost <script.txt> <out_dir>\n       kvr_simhost --peek <out_dir>\n"); return 2; }
    // --peek: save what the game is sending right now (eyes + panel) next to a running real host, touching nothing else
    bool peek = strcmp(argv[1], "--peek") == 0;
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_t0);
    SetConsoleCtrlHandler(OnConsoleClose, TRUE);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    FILE* sf = nullptr;
    if (!peek && fopen_s(&sf, argv[1], "r")) { printf("cannot open script %s\n", argv[1]); return 2; }
    g_out = argv[2];
    CreateDirectoryA(g_out.c_str(), nullptr);
    if (!peek) fopen_s(&g_csv, (g_out + "\\stats.csv").c_str(), "w");
    if (g_csv) fprintf(g_csv, "t,gameAlive,eye0,eye1,panel,world,cinematic,headYaw,headPitch\n");

    if (!OpenShared()) { printf("shared memory failed\n"); return 2; }
    // D3D11 on the NVIDIA adapter, same as the game's export device
    IDXGIFactory1* fac = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&fac));
    IDXGIAdapter1* chosen = nullptr;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; fac && fac->EnumAdapters1(i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        if (d.VendorId == 0x10DE && !chosen) { chosen = a; continue; }
        a->Release();
    }
    if (fac) fac->Release();
    ID3D11Device* dev = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(chosen, chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                                 D3D11_SDK_VERSION, &dev, nullptr, &g_ctx))) {
        printf("D3D11CreateDevice failed\n");
        return 2;
    }
    if (chosen) chosen->Release();
    dev->QueryInterface(IID_PPV_ARGS(&g_dev));
    dev->Release();
    if (peek) {
        if (!g_shm->texWidth) { printf("peek: the game has no shared textures yet\n"); return 1; }
        for (int i = 0; i < 200 && !(g_shared[0].have && g_shared[1].have && g_shared[2].have); i++) {
            PullAll();
            Sleep(10);
        }
        const char* names[3] = {"eye0", "eye1", "panel"};
        for (int i = 0; i < 3; i++)
            printf("peek: %s %s\n", names[i], SavePng(g_shared[i], g_out + "\\" + names[i] + ".png") ? "saved" : "missing");
        printf("peek: gameAlive %u lastWorldFrame %u cinematic %u showPanel %u\n", g_shm->gameAlive, g_shm->lastWorldFrame,
               g_shm->cinematic, g_shm->showPanel);
        return 0;
    }
    printf("kvr_simhost: simulated headset up, running %s -> %s\n", argv[1], argv[2]);

    int result = 0;
    char line[512];
    int lineNo = 0;
    while (!g_quit && fgets(line, sizeof line, sf)) {
        lineNo++;
        char* hash = strchr(line, '#');
        if (hash) *hash = 0;
        std::vector<std::string> tok;
        for (char* t = strtok(line, " \t\r\n"); t; t = strtok(nullptr, " \t\r\n")) tok.push_back(t);
        if (tok.empty()) continue;
        const std::string& cmd = tok[0];
        std::vector<std::string> args(tok.begin() + 1, tok.end());
        double v = 0;
        printf("[%.1f] %s", Now(), cmd.c_str());
        for (auto& s : args) printf(" %s", s.c_str());
        printf("\n");

        if (cmd == "end") break;
        else if (cmd == "wait") RunFor(args.empty() ? 1.0 : atof(args[0].c_str()));
        else if (cmd == "wait_menu" || cmd == "wait_world") {
            double timeout = args.empty() ? 60 : atof(args[0].c_str());
            bool world = cmd == "wait_world";
            bool ok = RunUntil(timeout, [&] { return world ? InWorld() : (g_shared[2].have && g_pulls[2] > 30); });
            printf("[%.1f]   %s\n", Now(), ok ? "ok" : "TIMED OUT");
            if (ok && world && !g_keepFocusOff) g_keepFocus = true;
            if (!ok) {  // leave evidence of where it got stuck
                const char* suffix[3] = {"eye0", "eye1", "panel"};
                for (int i = 0; i < 3; i++) SavePng(g_shared[i], g_out + "\\timeout_" + suffix[i] + ".png");
                result = 1;
                break;
            }
        } else if (cmd == "head") {
            g_headFrom = g_head;
            g_headTo = g_head;
            Arg(args, "yaw", g_headTo.yaw);
            Arg(args, "pitch", g_headTo.pitch);
            Arg(args, "x", g_headTo.x);
            Arg(args, "y", g_headTo.y);
            Arg(args, "z", g_headTo.z);
            g_headDur = 0;
            Arg(args, "over", g_headDur);
            g_headT0 = Now();
            if (g_headDur > 0) RunFor(g_headDur);
        } else if (cmd == "stick") {
            double x;
            if (Arg(args, "lx", x)) g_ctl.lx = (float)x;
            if (Arg(args, "ly", x)) g_ctl.ly = (float)x;
            if (Arg(args, "rx", x)) g_ctl.rx = (float)x;
            if (Arg(args, "ry", x)) g_ctl.ry = (float)x;
            if (Arg(args, "for", v)) { RunFor(v); g_ctl.lx = g_ctl.ly = g_ctl.rx = g_ctl.ry = 0; }
        } else if (cmd == "button") {
            uint32_t m = args.empty() ? 0 : ButtonMask(args[0]);
            if (!m) { printf("line %d: unknown button\n", lineNo); result = 2; break; }
            double hold = 0.15;
            Arg(args, "for", hold);
            g_ctl.buttons |= m;
            RunFor(hold);
            g_ctl.buttons &= ~m;
            RunFor(0.1);
        } else if (cmd == "click") {
            double u = 0.5, vv = 0.5, hold = 0.15;
            Arg(args, "u", u);
            Arg(args, "v", vv);
            Arg(args, "hold", hold);
            FocusGame();  // a window that stole the foreground swallows the click otherwise
            g_ctl.pointer = KVR_POINTER_PANEL;
            g_ctl.u = (float)u;
            g_ctl.v = (float)vv;
            RunFor(0.25);  // let the cursor settle on the target before pressing
            g_ctl.trigR = 1;
            RunFor(hold);
            g_ctl.trigR = 0;
            RunFor(0.2);
        } else if (cmd == "aimhand") {
            g_aimHand = true;
        } else if (cmd == "hand") {
            HandPose& hp = g_hand[(!args.empty() && (args[0] == "L" || args[0] == "l")) ? 0 : 1];
            Arg(args, "x", hp.x); Arg(args, "y", hp.y); Arg(args, "z", hp.z);
            Arg(args, "yaw", hp.yaw); Arg(args, "pitch", hp.pitch); Arg(args, "roll", hp.roll);
        } else if (cmd == "swing") {
            double dx = 0.6, dy = 0.3, dz = 0.0, over = 0.18;
            Arg(args, "dx", dx); Arg(args, "dy", dy); Arg(args, "dz", dz); Arg(args, "over", over);
            g_swingFrom = g_hand[1];
            g_swingTo = g_hand[1];
            g_swingTo.x += dx; g_swingTo.y += dy; g_swingTo.z += dz;
            g_swingDur = over;
            g_swingT0 = Now();
            RunFor(over + 0.1);
            g_hand[1] = g_swingFrom;  // back to rest
            RunFor(0.3);
        } else if (cmd == "trigger") {
            double hold = 0.15;
            Arg(args, "for", hold);
            g_ctl.trigR = 1;
            RunFor(hold);
            g_ctl.trigR = 0;
            RunFor(0.1);
        } else if (cmd == "record") {  // "record on" / "record off": video of the run (see RecordTick)
            if (!args.empty() && args[0] == "off") RecordStop();
            else RecordStart();
        } else if (cmd == "keepfocus") {
            g_keepFocusOff = !args.empty() && args[0] == "off";
            g_keepFocus = g_keepFocus && !g_keepFocusOff;
        } else if (cmd == "focus") {  // bring the game to the front (it stops rendering in the background)
            FocusGame();
            RunFor(0.5);
        } else if (cmd == "hover") {  // stand-in for the real host's ray hit-test: hover <panel> <line>
            if (args.size() >= 2) g_shm->text[atoi(args[0].c_str()) % KVR_TEXT_PANELS].hover = atoi(args[1].c_str());
            RunFor(0.1);
        } else if (cmd == "key") {
            std::string k = args.empty() ? "" : args[0];
            WORD vk = k == "grave" ? VK_OEM_3 : k == "enter" ? VK_RETURN : k == "escape" ? VK_ESCAPE : k == "space" ? VK_SPACE : k == "tab" ? VK_TAB : 0;
            if (!vk && k.size() >= 2 && (k[0] == 'f' || k[0] == 'F')) vk = (WORD)(VK_F1 + atoi(k.c_str() + 1) - 1);
            if (!vk && k.size() == 1 && isalnum((unsigned char)k[0])) vk = (WORD)toupper((unsigned char)k[0]);  // letters / digits
            if (!vk) { printf("line %d: unknown key\n", lineNo); result = 2; break; }
            FocusGame();
            TapKey(vk);
            RunFor(0.3);
        } else if (cmd == "type") {
            std::string text;  // the arguments re-joined with single spaces
            for (size_t i = 0; i < args.size(); i++) text += (i ? " " : "") + args[i];
            FocusGame();
            for (char c : text) {
                TypeChar(c);
                RunFor(0.04);
            }
            RunFor(0.2);
        } else if (cmd == "wait_calm") {
            // until the panel has delivered frames without a gap > 0.5 s for <seconds> (menus: before clicking)
            double need = args.empty() ? 3 : atof(args[0].c_str());
            double timeout = args.size() > 1 ? atof(args[1].c_str()) : 90;
            bool ok = RunUntil(timeout, [&] {
                double now = Now();
                return g_calmSince >= 0 && now - g_lastPanelPull < 0.5 && now - g_calmSince >= need;
            });
            printf("[%.1f]   %s\n", Now(), ok ? "calm" : "never calm (continuing)");
        } else if (cmd == "wait_log" || cmd == "wait_new_log") {
            std::string needle = args.empty() ? "" : args[0];
            for (auto& ch : needle)
                if (ch == '~') ch = ' ';  // '~' stands for a space
            double timeout = args.size() > 1 ? atof(args[1].c_str()) : 60;
            long from = cmd == "wait_new_log" ? LogSize() : 0;
            bool ok = RunUntil(timeout, [&] { return LogContains(needle, from); });
            printf("[%.1f]   %s\n", Now(), ok ? "found" : "NOT FOUND");
            if (!ok && cmd == "wait_new_log") {
                printf("[%.1f]   (wait_new_log is a sync point, not a check: continuing)\n", Now());
            } else if (!ok) {
                const char* suffix[3] = {"eye0", "eye1", "panel"};
                for (int i = 0; i < 3; i++) SavePng(g_shared[i], g_out + "\\timeout_" + suffix[i] + ".png");
                result = 1;
                break;
            }
        } else if (cmd == "squeeze") {
            // hold analog grip / trigger on one hand until changed (finger curl checks); no click is implied
            bool left = !args.empty() && (args[0] == "L" || args[0] == "l");
            double g = left ? g_ctl.gripL : g_ctl.gripR, t = left ? g_ctl.trigL : g_ctl.trigR;
            Arg(args, "grip", g);
            Arg(args, "trig", t);
            (left ? g_ctl.gripL : g_ctl.gripR) = (float)g;
            (left ? g_ctl.trigL : g_ctl.trigR) = (float)t;
        } else if (cmd == "grip") {
            double hold = 0.2;
            Arg(args, "for", hold);
            g_ctl.gripR = 1;
            RunFor(hold);
            g_ctl.gripR = 0;
            RunFor(0.1);
        } else if (cmd == "pointer") {
            // "pointer none" stops pointing; "pointer world" points the right hand where the head looks
            // "pointer hud u= v=": the LEFT hand on the HUD in the world (the left trigger clicks it)
            g_ctl.pointer = (!args.empty() && args[0] == "world") ? KVR_POINTER_WORLD : KVR_POINTER_NONE;
            if (!args.empty() && args[0] == "hud") {
                double u = 0.5, vv = 0.5;
                Arg(args, "u", u);
                Arg(args, "v", vv);
                g_ctl.pointer = KVR_POINTER_HUD_LEFT;
                g_ctl.u = (float)u;
                g_ctl.v = (float)vv;
            }
        } else if (cmd == "triggerl") {  // left trigger tap
            g_ctl.trigL = 1;
            RunFor(0.15);
            g_ctl.trigL = 0;
            RunFor(0.1);
        } else if (cmd == "shot") {
            std::string name = args.empty() ? "shot" : args[0];
            RunFor(0.3);  // a few fresh frames of both eyes at the current pose
            const char* suffix[3] = {"eye0", "eye1", "panel"};
            for (int i = 0; i < 3; i++) {
                std::string p = g_out + "\\" + name + "_" + suffix[i] + ".png";
                bool ok = SavePng(g_shared[i], p);
                printf("[%.1f]   %s %s\n", Now(), p.c_str(), ok ? "saved" : "(no frame)");
            }
        } else if (cmd == "game") {
            std::string text;
            for (size_t i = 0; i < args.size(); i++) text += (i ? " " : "") + args[i];
            FILE* gf = nullptr;
            if (fopen_s(&gf, GameCmdPath().c_str(), "a") == 0 && gf) {
                fprintf(gf, "%s\n", text.c_str());
                fclose(gf);
            }
            printf("[%.1f]   -> game: %s\n", Now(), text.c_str());
        } else if (cmd == "hand_adjust") {  // turn the right hand by the game's last "aimhint dyaw= dpitch=" line
            FILE* f = _fsopen(kProbeLog, "rb", _SH_DENYNO);
            std::string all;
            if (f) {
                fseek(f, 0, SEEK_END);
                long n = ftell(f), from = n > 65536 ? n - 65536 : 0;
                all.resize(n - from);
                fseek(f, from, SEEK_SET);
                all.resize(fread(&all[0], 1, all.size(), f));
                fclose(f);
            }
            size_t at = all.rfind("aimhint dyaw=");
            float dy = 0, dp = 0;
            if (at != std::string::npos && sscanf_s(all.c_str() + at, "aimhint dyaw=%f dpitch=%f", &dy, &dp) == 2) {
                g_hand[1].yaw += dy;
                g_hand[1].pitch += dp;
                printf("[%.1f]   hand R turned by yaw %.2f pitch %.2f -> yaw %.2f pitch %.2f\n", Now(), dy, dp, g_hand[1].yaw,
                       g_hand[1].pitch);
            } else {
                printf("[%.1f]   hand_adjust: no aimhint in the log\n", Now());
            }
        } else if (cmd == "check_bright") {  // CHECK: the left eye's mean brightness reaches <min> within <timeout> s
            std::string name = args.size() > 0 ? args[0] : "?";
            double minL = args.size() > 1 ? atof(args[1].c_str()) : 10, timeout = args.size() > 2 ? atof(args[2].c_str()) : 5;
            double t0 = Now(), l = -1;
            bool pass = RunUntil(timeout, [&] { l = MeanLuma(g_shared[0]); return l >= minL; });
            printf("[%.1f]   CHECK %s %s (eye brightness %.1f after %.1f s, want >= %.1f)\n", Now(), pass ? "PASS" : "FAIL",
                   name.c_str(), l, Now() - t0, minL);
            if (!pass) g_checkFailed = true;
        } else if (cmd == "check_dark_time") {  // CHECK: over <seconds>, the eye is darker than <min> for <= <maxDark> s
            std::string name = args.size() > 0 ? args[0] : "?";
            double minL = args.size() > 1 ? atof(args[1].c_str()) : 10, span = args.size() > 2 ? atof(args[2].c_str()) : 10;
            double maxDark = args.size() > 3 ? atof(args[3].c_str()) : 1;
            double until = Now() + span, dark = 0, lowest = 1e9;
            bool wasDark = false;
            int episode = 0;
            while (Now() < until) {
                RunFor(0.1);
                double l = MeanLuma(g_shared[0]);
                bool isDark = l >= 0 && l < minL;
                if (isDark) dark += 0.1;
                if (isDark && !wasDark && episode < 20) {  // where it happened: the game logs its position; a picture
                    FILE* gf = nullptr;
                    if (fopen_s(&gf, GameCmdPath().c_str(), "a") == 0 && gf) {
                        fprintf(gf, "pos\n");
                        fclose(gf);
                    }
                    char nm[64];
                    sprintf_s(nm, "\\dark_%s_%d_eye0.png", name.c_str(), episode++);
                    SavePng(g_shared[0], g_out + nm);
                    printf("[%.1f]   dark (%.1f) - asked the game for its position, saved%s\n", Now(), l, nm);
                }
                wasDark = isDark;
                if (l >= 0 && l < lowest) lowest = l;
            }
            bool pass = dark <= maxDark;
            printf("[%.1f]   CHECK %s %s (dark %.1f s of %.0f s, darkest %.1f, want <= %.1f s below %.1f)\n", Now(),
                   pass ? "PASS" : "FAIL", name.c_str(), dark, span, lowest, maxDark, minL);
            if (!pass) g_checkFailed = true;
        } else if (cmd == "lumatrace") {  // the left eye's brightness every 0.25 s for <seconds> (diagnostics)
            double until = Now() + (args.empty() ? 5 : atof(args[0].c_str()));
            while (Now() < until) {
                RunFor(0.25);
                printf("[%.1f]   luma %.1f\n", Now(), MeanLuma(g_shared[0]));
            }
        } else if (cmd == "mark") {  // check_since_mark looks from here (an event logged before its check started)
            g_mark = LogSize();
        } else if (cmd == "check_log" || cmd == "check_no_log" || cmd == "check_count" || cmd == "check_since_mark") {
            // "@mark" as the first argument: count from the last mark instead of from now
            bool fromMark = cmd == "check_since_mark" || (!args.empty() && args[0] == "@mark");
            if (!args.empty() && args[0] == "@mark") args.erase(args.begin());
            std::string name = args.size() > 0 ? args[0] : "?", needle = args.size() > 1 ? args[1] : "";
            for (auto& ch : needle)
                if (ch == '~') ch = ' ';
            long from = fromMark ? g_mark : LogSize();
            bool pass;
            char detail[96] = "";
            if (cmd == "check_log" || cmd == "check_since_mark") {
                pass = RunUntil(args.size() > 2 ? atof(args[2].c_str()) : 30, [&] { return LogContains(needle, from); });
            } else if (cmd == "check_no_log") {
                RunFor(args.size() > 2 ? atof(args[2].c_str()) : 10);
                int c = LogCount(needle, from);
                pass = c == 0;
                sprintf_s(detail, "(%d seen)", c);
            } else {
                int lo = args.size() > 2 ? atoi(args[2].c_str()) : 0, hi = args.size() > 3 ? atoi(args[3].c_str()) : 0;
                RunFor(args.size() > 4 ? atof(args[4].c_str()) : 10);
                int c = LogCount(needle, from);
                pass = c >= lo && c <= hi;
                sprintf_s(detail, "(%d seen, want %d..%d)", c, lo, hi);
            }
            printf("[%.1f]   CHECK %s %s %s\n", Now(), pass ? "PASS" : "FAIL", name.c_str(), detail);
            if (!pass) g_checkFailed = true;
        } else if (cmd == "expect_world") {
            bool ok = InWorld();
            printf("[%.1f]   expect_world: %s\n", Now(), ok ? "ok" : "FAILED");
            if (!ok) { result = 1; break; }
        } else {
            printf("line %d: unknown command '%s'\n", lineNo, cmd.c_str());
            result = 2;
            break;
        }
    }
    fclose(sf);
    if (result == 0 && g_checkFailed) result = 1;
    g_shm->input.active = 0;
    g_shm->host.running = 0;
    if (g_csv) fclose(g_csv);
    RecordStop();
    printf("kvr_simhost: %s (frames pulled: eye0 %u, eye1 %u, panel %u)\n", result == 0 ? "script completed" : "script FAILED",
           g_pulls[0], g_pulls[1], g_pulls[2]);
    return result;
}
