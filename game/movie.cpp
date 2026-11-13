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
const WNDPROC kGameMovieProc = (WNDPROC)0x405190;  // the movie window's procedure (FINDINGS "Cutscene movies")

LRESULT CALLBACK MovieWndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ACTIVATEAPP && wp == 0) {  // losing the foreground must not interrupt the movie
        static int n;
        if (n++ < 20) Log("movie: app deactivation dropped (window %p, frame %u)", w, g_frames);
        return 0;
    }
    return CallWindowProcA(kGameMovieProc, w, msg, wp, lp);
}

// Why the movie ended, from the player object [0x7A3CF4] the window procedure 0x405190 writes: +0x1C/+0x20 interrupted
// (focus 0 / Alt+Enter 1; the game replays it later and the module waits for it), +0x14/+0x18 stopped / skipped.
void LogEnd() {
    __try {
        BYTE* p = *(BYTE**)0x7A3CF4;
        if (!p) {
            Log("movie: end state unknown (no player object)");
            return;
        }
        int stop = *(int*)(p + 0x14), param = *(int*)(p + 0x18), intr = *(int*)(p + 0x1C), how = *(int*)(p + 0x20);
        Log("movie: end state stop %d param %d interrupted %d (%s)", stop, param, intr,
            !intr ? "-" : how ? "Alt+Enter" : "focus lost - the game replays it later");
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
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
    g_subclassed = f.w;
    if (prev == MovieWndProc) {  // created from the patched class: guarded from its first message
        Log("movie: window %p guarded from creation", f.w);
        return;
    }
    // always hand on to the game's own procedure (RegisterClassA in 0x4053E0), never to whatever the window had:
    // with the class patched, a hook that wrapped it would otherwise call back into us
    SetWindowLongPtrA(f.w, GWLP_WNDPROC, (LONG_PTR)MovieWndProc);
    // Subclassing on the first decoded frame is too late when the window is deactivated as it opens (playtest
    // 2026-11-13: the second of two back-to-back Taris escape movies, 05_1C, was interrupted after 1 frame; STUNT_07
    // then waited ~100 s for its replay with the turret cutscene held back). Every later movie window gets our
    // procedure from its creation.
    static bool classPatched;
    if (!classPatched) {
        classPatched = true;
        SetLastError(0);
        ULONG_PTR was = SetClassLongPtrA(f.w, GCLP_WNDPROC, (LONG_PTR)MovieWndProc);
        Log("movie: class procedure %p -> ours (error %lu); window had %p", (void*)was, GetLastError(), (void*)prev);
    }
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
    if (g_playing) {
        Log("movie: closed after %u frames t=%lu", g_frames, GetTickCount());
        LogEnd();
    }
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
