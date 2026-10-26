// Cutscene movies in the headset (see movie.h).
//
// KotOR plays its Bink movies (binkw32.dll, imported by swkotor.exe) in a loop of BinkDoFrame -> BinkCopyToBuffer ->
// BinkBufferBlit into a separate "SW Movie Player Window", with no OpenGL frames in between. We hook BinkDoFrame and
// BinkClose through swkotor's import table: after each decoded frame we copy it once more, as 32-bit pixels, into our
// own buffer and hand it to the export (VR_MovieFrame), which sends it to the host in the panel texture flagged
// KVR_SLOT_MOVIE; the host then shows it as a cinema screen. Controller A / B during a movie skip it (Escape).
//
// The movie window's procedure (0x405190) stops a movie as "interrupted" when the app is deactivated
// (WM_ACTIVATEAPP 0), and the game then replays it later from the start. In VR the game is rarely the active app
// (headset runtime, host console, desktop), so we subclass each movie window and drop that message.
#include "movie.h"

#include <cstring>
#include <vector>

#include "../common/kvr_shared.h"
#include "vr.h"

extern void Log(const char* fmt, ...);

namespace {

typedef int(__stdcall* DoFrameFn)(void* bink);
typedef void(__stdcall* CloseFn)(void* bink);
typedef int(__stdcall* CopyToBufferFn)(void* bink, void* dest, int pitch, unsigned height, unsigned x, unsigned y,
                                       unsigned flags);

const unsigned kSurface32 = 3;             // BINKSURFACE32: B, G, R, x in memory
const unsigned kCopyAll = 0x80000000u;     // BINKCOPYALL: the whole frame, not just the changed rectangles

DoFrameFn r_DoFrame;
CloseFn r_Close;
CopyToBufferFn p_CopyToBuffer;
std::vector<unsigned char> g_px;
bool g_playing;
unsigned g_frames;
bool g_skipWas;
HWND g_subclassed;
WNDPROC g_movieProc;

LRESULT CALLBACK MovieWndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ACTIVATEAPP && wp == 0) return 0;  // losing the foreground must not interrupt the movie
    return CallWindowProcA(g_movieProc, w, msg, wp, lp);
}

void SubclassMovieWindow() {
    // the movie thread (this one) owns the window it just created for this movie
    struct Find { DWORD tid; HWND w; } f = {GetCurrentThreadId(), nullptr};
    EnumThreadWindows(f.tid, [](HWND w, LPARAM p) -> BOOL {
        char cls[32];
        if (GetClassNameA(w, cls, sizeof cls) && !strcmp(cls, "SWMovieWindow")) {
            ((Find*)p)->w = w;
            return FALSE;
        }
        return TRUE;
    }, (LPARAM)&f);
    if (!f.w || f.w == g_subclassed) return;
    WNDPROC prev = (WNDPROC)GetWindowLongPtrA(f.w, GWLP_WNDPROC);
    if (prev == MovieWndProc) return;
    g_movieProc = prev;
    SetWindowLongPtrA(f.w, GWLP_WNDPROC, (LONG_PTR)MovieWndProc);
    g_subclassed = f.w;
    Log("movie: window %p kept playing through focus changes", f.w);
}

void SkipIfAsked(void* bink) {
    const KvrShared* shm = VR_Shared();
    if (!shm) return;
    bool skip = (shm->input.buttons & (KVR_BTN_A | KVR_BTN_B)) != 0;
    if (skip && !g_skipWas) {
        // the movie loop takes Escape from its window's messages
        HWND w = g_subclassed ? g_subclassed : FindWindowA("SWMovieWindow", nullptr);
        if (w) {
            PostMessageA(w, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
            PostMessageA(w, WM_KEYUP, VK_ESCAPE, 0xC0010001);
        }
        Log("movie: skip requested (window %p)", w);
    }
    g_skipWas = skip;
}

int __stdcall h_DoFrame(void* bink) {
    int r = r_DoFrame(bink);
    unsigned w = 0, h = 0;
    __try {
        w = *(unsigned*)bink;
        h = *((unsigned*)bink + 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (w && h && w <= 4096 && h <= 4096 && p_CopyToBuffer) {
        if (!g_playing) {
            g_playing = true;
            g_frames = 0;
            Log("movie: playing %ux%u t=%lu", w, h, GetTickCount());
            SubclassMovieWindow();
        }
        g_px.resize((size_t)w * h * 4);
        p_CopyToBuffer(bink, g_px.data(), (int)w * 4, h, 0, 0, kSurface32 | kCopyAll);
        VR_MovieFrame(g_px.data(), (int)w, (int)h);
        g_frames++;
        SkipIfAsked(bink);
    }
    return r;
}

void __stdcall h_Close(void* bink) {
    if (g_playing) Log("movie: closed after %u frames t=%lu", g_frames, GetTickCount());
    g_playing = false;
    VR_MovieEnded();
    r_Close(bink);
}

// Point swkotor's import slot for `real` at `hook`; returns the original.
void* PatchImport(void* real, void* hook) {
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        for (auto* t = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk); t->u1.Function; t++) {
            if ((void*)t->u1.Function != real) continue;
            DWORD old;
            VirtualProtect(&t->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            t->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&t->u1.Function, sizeof(void*), old, &old);
            return real;
        }
    }
    return nullptr;
}

}  // namespace

bool Movie_Install() {
    HMODULE bink = GetModuleHandleA("binkw32.dll");
    if (!bink) {
        Log("movie: binkw32.dll not loaded");
        return false;
    }
    void* doFrame = GetProcAddress(bink, "_BinkDoFrame@4");
    void* close = GetProcAddress(bink, "_BinkClose@4");
    p_CopyToBuffer = (CopyToBufferFn)GetProcAddress(bink, "_BinkCopyToBuffer@28");
    r_DoFrame = (DoFrameFn)PatchImport(doFrame, (void*)h_DoFrame);
    r_Close = (CloseFn)PatchImport(close, (void*)h_Close);
    Log("movie: BinkDoFrame %s, BinkClose %s", r_DoFrame ? "hooked" : "NOT hooked", r_Close ? "hooked" : "NOT hooked");
    if (!r_Close && close) r_Close = (CloseFn)close;
    return r_DoFrame && r_Close;
}

bool Movie_Playing() { return g_playing; }
