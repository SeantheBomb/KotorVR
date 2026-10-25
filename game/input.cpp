// Controller input injection (x86, inside swkotor.exe).
//
// The game reads keyboard and mouse through DirectInput 8 and keeps its own cursor, which it mirrors to the
// hardware cursor with SetCursorPos. We wrap the IDirectInput8A it creates; its keyboard and mouse devices get
// controller-derived keys, buttons and mouse deltas merged into both immediate (GetDeviceState) and buffered
// (GetDeviceData) reads. The cursor is steered to the laser target by injecting deltas, using the game's own
// SetCursorPos calls as feedback (it scales deltas by its sensitivity setting, so the gain self-calibrates).

#define DIRECTINPUT_VERSION 0x0800
#include "input.h"

#include <dinput.h>
#include <cmath>
#include <cstring>

#include "../common/kvr_shared.h"
#include "vr.h"
#include "gestures.h"
#include "weapons.h"
#include "features.h"

#pragma comment(lib, "dxguid.lib")

extern void Log(const char* fmt, ...);

// ---------------------------------------------------------------- desired state (computed per frame)
static BYTE g_wantKeys[256];   // DIK_* -> 0x80 when held
static bool g_wantMouse[2];    // left, right
static LONG g_pendDx, g_pendDy;  // mouse delta not yet delivered to the game
static CRITICAL_SECTION g_cs;
static bool g_csInit;

static void Lock() {
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; }
    EnterCriticalSection(&g_cs);
}
static void Unlock() { LeaveCriticalSection(&g_cs); }

// cursor feedback
static bool g_haveCursor;
static POINT g_cursorScreen;
static DWORD g_cursorStamp;   // tick of the last game SetCursorPos
static float g_gain = 1.0f;   // observed cursor pixels per injected mickey
static LONG g_lastInjX, g_lastInjY;
static POINT g_lastCursorAtInject;
static DWORD g_lastInjectTick;

// absolute cursor path: if the game reads the hardware cursor via GetCursorInfo we just hand it the target
static bool g_targetValid;
static bool g_msgButtons[2];  // button state last posted as window messages

// recent cursor targets, so a melee swing can attack what the hand pointed at just before the swing moved it
struct TargetSample {
    double t;
    POINT scr;
    float tx, ty;
};
static TargetSample g_targetHist[64];
static int g_targetHead;
// scripted click: move to (tx, ty) and click once the GUI has seen the hover
static int g_autoClickFrames;  // frames remaining (press during the middle of the run)
static float g_autoTx, g_autoTy;
// keys pressed by gestures for a few frames
// gesture key press: held for a fixed time (a few frames is only ~16 ms at 250 fps), then released
static double g_gestureKeyAt = -1;
static BYTE g_gestureKey;
static POINT g_targetScreen;
static DWORD g_lastCursorInfoTick;
static volatile LONG g_nCursorInfo, g_nSetCursor, g_nMouseState, g_nMouseData;

void Input_OnGameGetCursorInfo(PCURSORINFO ci) {
    InterlockedIncrement(&g_nCursorInfo);
    g_lastCursorInfoTick = GetTickCount();
    if (g_targetValid) ci->ptScreenPos = g_targetScreen;
}

void Input_OnGameSetCursorPos(int x, int y) {
    InterlockedIncrement(&g_nSetCursor);
    g_cursorScreen = {x, y};
    g_haveCursor = true;
    g_cursorStamp = GetTickCount();
}

static bool ReadInput(KvrInput& out) {
    const KvrShared* shm = VR_Shared();
    if (!shm) return false;
    for (int tries = 0; tries < 100; tries++) {
        uint32_t s1 = shm->input.seq;
        if (s1 & 1) continue;
        MemoryBarrier();
        memcpy(&out, (const void*)&shm->input, sizeof out);
        MemoryBarrier();
        if (shm->input.seq == s1) return true;
    }
    return false;
}

void Input_Update(HWND wnd) {
    KvrInput in{};
    BYTE keys[256] = {};
    bool useDiButtons = false;
    bool mouse[2] = {false, false};
    LONG dx = 0, dy = 0;
    if (ReadInput(in) && in.active) {
        // KotOR ignores GUI input while it is not the foreground app. When the player is actively using the
        // controllers and something else took focus (Meta dashboard, a desktop window), take it back.
        bool using_ = in.buttons || in.trigR > 0.3f || in.trigL > 0.3f || in.gripR > 0.3f || in.gripL > 0.3f ||
                      fabsf(in.lx) > 0.3f || fabsf(in.ly) > 0.3f || fabsf(in.rx) > 0.3f;
        HWND root = wnd ? GetAncestor(wnd, GA_ROOT) : nullptr;
        static DWORD lastActivate;
        if (using_ && root && GetForegroundWindow() != root && GetTickCount() - lastActivate > 2000) {
            // Don't steal real focus (attaching to another app's input queue can block for many seconds); just
            // tell the game it is active so its GUI accepts our posted input.
            lastActivate = GetTickCount();
            // Windows lets the process that produced the last input event take the foreground: tap Alt (no
            // effect on its own) and bring the game forward. Also tell the game it is active, in case Windows
            // still refuses (the GUI then at least accepts our posted input).
            INPUT alt[2] = {};
            alt[0].type = alt[1].type = INPUT_KEYBOARD;
            alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
            alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, alt, sizeof(INPUT));
            SetForegroundWindow(root);
            PostMessageA(root, WM_ACTIVATEAPP, TRUE, 0);
            PostMessageA(root, WM_ACTIVATE, WA_ACTIVE, 0);
            static int told;
            if (told++ < 3) Log("input: game is in the background; posted activation so controller input still lands");
        }
        // movement: left stick in head space -> rotate into the game camera's frame -> W/S forward/back, Z/C strafe
        // (KotOR keymap.2da). KotOR moves relative to its camera, which only turns with the right stick, so without
        // this the stick would steer like tank controls whenever you look around.
        float psi = VR_HeadYaw();
        float c = cosf(psi), sn = sinf(psi);
        float mr = in.lx * c - in.ly * sn;  // right component in the game camera frame
        float mf = in.lx * sn + in.ly * c;  // forward component
        float mag = sqrtf(mr * mr + mf * mf);
        if (mag > 0.5f) {
            // 8-way: a component counts once it is at least ~38% of the push (22.5 deg sectors)
            if (mf > 0.38f * mag) keys[DIK_W] = 0x80;
            if (mf < -0.38f * mag) keys[DIK_S] = 0x80;
            if (mr < -0.38f * mag) keys[DIK_Z] = 0x80;
            if (mr > 0.38f * mag) keys[DIK_C] = 0x80;
        }
        // camera rotate: right stick X -> A/D
        if (in.rx < -0.6f) keys[DIK_A] = 0x80;
        if (in.rx > 0.6f) keys[DIK_D] = 0x80;
        // ---- motion gestures (melee swings, shots, Force moves) and the attacks they trigger
        float headPos[3] = {0, 0, 0};
        VR_HeadPos(headPos);
        Gestures_Update(in, headPos);
        double now = Gestures_Now();
        float swingQ;
        // Attacks go through KotOR's Default Action (R) on the current target: clicking would make the character
        // walk to the floor when the swing / shot isn't over an enemy. Grip selects / uses what the hand points at.
        bool inWorld = in.pointerMode == KVR_POINTER_WORLD && Features().gestures;
        bool fighting = inWorld && Weapon_PlayerInCombat();  // R outside combat would *use* whatever is targeted
        if (Gestures_TakeSwing(&swingQ) && fighting) {
            g_gestureKey = DIK_R;
            g_gestureKeyAt = now;
            Log("input: swing -> attack current target");
        }
        static bool trigWas;
        bool trigNow = in.trigR > 0.6f;
        if (trigNow && !trigWas && fighting) {
            if (!Weapon_Profile(Weapon_Right()).melee) Gestures_OnShot();
            g_gestureKey = DIK_R;
            g_gestureKeyAt = now;
        }
        trigWas = trigNow;
        if (Gestures_TakeForcePush() && fighting) { g_gestureKey = DIK_2; g_gestureKeyAt = now; }  // target slot 2: Force powers
        if (Gestures_TakeForcePull() && inWorld) { g_gestureKey = DIK_4; g_gestureKeyAt = now; }  // personal power slot
        if (g_gestureKeyAt >= 0) {
            if (now - g_gestureKeyAt < 0.1) keys[g_gestureKey] = 0x80;
            else g_gestureKeyAt = -1;
        }

        if (in.buttons & KVR_BTN_A) keys[DIK_SPACE] = 0x80;                     // pause
        if (in.buttons & (KVR_BTN_B | KVR_BTN_MENU)) keys[DIK_ESCAPE] = 0x80;   // menu / back
        if (in.buttons & KVR_BTN_X) keys[DIK_TAB] = 0x80;                       // change character
        // click: trigger on the HUD; in the world either trigger or a grip squeeze uses / talks to / attacks the
        // object your right hand points at (the game highlights it through its own hover feedback)
        bool world = in.pointerMode == KVR_POINTER_WORLD;
        mouse[0] = (!world && in.trigR > 0.6f) || (world && in.gripR > 0.7f);
        mouse[1] = in.trigL > 0.6f;

        // cursor steering toward the pointer target
        float tx, ty;
        bool haveTarget = false;
        if (in.pointerMode == KVR_POINTER_PANEL) {
            int w, h;
            VR_ClientSize(&w, &h);
            tx = in.panelU * w;
            ty = in.panelV * h;
            haveTarget = w > 0;
        } else if (in.pointerMode == KVR_POINTER_WORLD) {
            haveTarget = VR_WorldPointerToClient(in.aim.pos, in.aim.quat, &tx, &ty);
        }
        if (g_autoClickFrames > 0) {
            g_autoClickFrames--;
            tx = g_autoTx;
            ty = g_autoTy;
            haveTarget = true;
            mouse[0] = g_autoClickFrames <= 6 && g_autoClickFrames >= 3;  // hold a few frames after the hover settles
        } else if (haveTarget && in.pointerMode == KVR_POINTER_WORLD) {
            g_targetHist[g_targetHead] = {now, {0, 0}, tx, ty};
            g_targetHead = (g_targetHead + 1) % 64;
        }
        g_targetValid = false;
        if (haveTarget && wnd) {
            POINT t = {(LONG)lroundf(tx), (LONG)lroundf(ty)};
            ClientToScreen(wnd, &t);
            g_targetScreen = t;
            g_targetValid = true;
        }
        // The game takes the mouse from window messages (its DirectInput mouse is never read), so place the real
        // cursor and post the matching messages. Buttons go out as messages too, on press/release edges.
        if (g_targetValid) {
            // Clicks reach the GUI only as window messages (the WndProc converts WM_*BUTTON* into GUI events), so
            // post them to the game window actually under the cursor, in that window's client coordinates.
            POINT scr = g_targetScreen;
            HWND hit = WindowFromPoint(scr);
            DWORD pid = 0;
            if (hit) GetWindowThreadProcessId(hit, &pid);
            HWND dst = (hit && pid == GetCurrentProcessId()) ? hit : wnd;
            POINT client = scr;
            ScreenToClient(dst, &client);
            static POINT lastScr = {-1, -1};
            static int settledFrames;  // frames since the cursor last moved
            WPARAM held = (g_msgButtons[0] ? MK_LBUTTON : 0) | (g_msgButtons[1] ? MK_RBUTTON : 0);
            if (scr.x != lastScr.x || scr.y != lastScr.y) {
                SetCursorPos(scr.x, scr.y);
                PostMessageA(dst, WM_MOUSEMOVE, held, MAKELPARAM(client.x, client.y));
                // only a real jump resets the settle count; hand tremor of a pixel or two must not block clicks
                if (labs(scr.x - lastScr.x) + labs(scr.y - lastScr.y) > 6) settledFrames = 0;
                lastScr = scr;
            }
            settledFrames++;
            for (int b = 0; b < 2; b++) {
                // a press must arrive after the GUI has seen the hover at this spot (it ignores press-on-arrival)
                if (mouse[b] && !g_msgButtons[b] && settledFrames < 3) continue;
                if (mouse[b] != g_msgButtons[b]) {
                    g_msgButtons[b] = mouse[b];
                    WPARAM nowHeld = (g_msgButtons[0] ? MK_LBUTTON : 0) | (g_msgButtons[1] ? MK_RBUTTON : 0);
                    UINT msg = b == 0 ? (mouse[b] ? WM_LBUTTONDOWN : WM_LBUTTONUP) : (mouse[b] ? WM_RBUTTONDOWN : WM_RBUTTONUP);
                    BOOL ok = PostMessageA(dst, msg, nowHeld, MAKELPARAM(client.x, client.y));
                    Log("input: %s %s -> hwnd %p (%s, render wnd %p) client (%ld,%ld) posted=%d", b ? "right" : "left",
                        mouse[b] ? "down" : "up", dst, dst == hit ? "under cursor" : "fallback", wnd, client.x, client.y, ok);
                }
            }
        }
        bool absolutePath = true;  // message path above; the DirectInput delta path stays off
        if (haveTarget && g_haveCursor && wnd && !absolutePath) {
            POINT c = g_cursorScreen;
            ScreenToClient(wnd, &c);
            // learn the gain from how far the game moved its cursor after the last injection
            if (g_lastInjectTick && g_cursorStamp > g_lastInjectTick) {
                LONG ox = g_cursorScreen.x - g_lastCursorAtInject.x, oy = g_cursorScreen.y - g_lastCursorAtInject.y;
                LONG ix = g_lastInjX, iy = g_lastInjY;
                if (labs(ix) + labs(iy) >= 6 && labs(ox) + labs(oy) > 0) {
                    float r = (float)(labs(ox) + labs(oy)) / (float)(labs(ix) + labs(iy));
                    if (r > 0.05f && r < 20.0f) g_gain = 0.8f * g_gain + 0.2f * r;
                }
                g_lastInjectTick = 0;
            }
            float ex = tx - c.x, ey = ty - c.y;
            // don't keep pushing if the game stopped reporting its cursor (it would run away)
            bool fresh = GetTickCount() - g_cursorStamp < 500 || g_lastInjectTick == 0;
            if (fresh && (fabsf(ex) > 1.5f || fabsf(ey) > 1.5f)) {
                dx = (LONG)lroundf(ex / g_gain);
                dy = (LONG)lroundf(ey / g_gain);
                dx = dx > 300 ? 300 : dx < -300 ? -300 : dx;
                dy = dy > 300 ? 300 : dy < -300 ? -300 : dy;
                g_lastInjX = dx;
                g_lastInjY = dy;
                g_lastCursorAtInject = g_cursorScreen;
                g_lastInjectTick = GetTickCount();
            }
        }
    }
    static DWORD lastDbg;
    if (GetTickCount() - lastDbg > 1000) {
        lastDbg = GetTickCount();
        Log("input: fg %d | ptr mode %u target %s (%ld,%ld) | game cursor %s (%ld,%ld) | GetCursorInfo %ld SetCursorPos %ld mouseState %ld mouseData %ld | gain %.2f inj (%ld,%ld)",
            wnd && GetForegroundWindow() == GetAncestor(wnd, GA_ROOT), in.active ? in.pointerMode : 0, g_targetValid ? "yes" : "no", g_targetScreen.x, g_targetScreen.y,
            g_haveCursor ? "seen" : "never", g_cursorScreen.x, g_cursorScreen.y, g_nCursorInfo, g_nSetCursor, g_nMouseState,
            g_nMouseData, g_gain, dx, dy);
    }
    Lock();
    memcpy(g_wantKeys, keys, sizeof keys);
    g_wantMouse[0] = false;  // buttons only as window messages (see above)
    g_wantMouse[1] = false;
    g_pendDx = dx;  // replace, not accumulate: recomputed from fresh feedback every frame
    g_pendDy = dy;
    Unlock();
}

// ---------------------------------------------------------------- device wrapper
enum DevKind { DEV_KEYBOARD, DEV_MOUSE };

class WrapDevice : public IDirectInputDevice8A {
public:
    WrapDevice(IDirectInputDevice8A* real, DevKind kind) : m_real(real), m_kind(kind) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        HRESULT hr = m_real->QueryInterface(riid, out);
        if (SUCCEEDED(hr) && *out == m_real) { *out = this; }
        return hr;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_real->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = m_real->Release();
        if (n == 0) delete this;
        return n;
    }

    // the two reads we augment
    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD cb, LPVOID data) override {
        HRESULT hr = m_real->GetDeviceState(cb, data);
        if (!data) return hr;
        if (FAILED(hr)) {
            // background: the device can't be acquired; deliver only the controller state instead of failing
            if (hr != DIERR_NOTACQUIRED && hr != DIERR_INPUTLOST) return hr;
            memset(data, 0, cb);
            hr = DI_OK;
        }
        Lock();
        if (m_kind == DEV_KEYBOARD && cb >= 256) {
            BYTE* k = (BYTE*)data;
            for (int i = 0; i < 256; i++) k[i] |= g_wantKeys[i];
        } else if (m_kind == DEV_MOUSE && cb >= sizeof(DIMOUSESTATE)) {
            InterlockedIncrement(&g_nMouseState);
            DIMOUSESTATE* m = (DIMOUSESTATE*)data;
            m->lX += g_pendDx;
            m->lY += g_pendDy;
            g_pendDx = g_pendDy = 0;
            if (g_wantMouse[0]) m->rgbButtons[0] |= 0x80;
            if (g_wantMouse[1]) m->rgbButtons[1] |= 0x80;
        }
        Unlock();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD cbObj, LPDIDEVICEOBJECTDATA rgdod, LPDWORD inOut, DWORD flags) override {
        DWORD capacity = inOut ? *inOut : 0;
        HRESULT hr = m_real->GetDeviceData(cbObj, rgdod, inOut, flags);
        if (FAILED(hr) || !rgdod || !inOut || (flags & DIGDD_PEEK) || cbObj < sizeof(DIDEVICEOBJECTDATA)) return hr;
        DWORD n = *inOut;
        auto push = [&](DWORD ofs, DWORD value) {
            if (n >= capacity) return;
            DIDEVICEOBJECTDATA* e = (DIDEVICEOBJECTDATA*)((BYTE*)rgdod + n * cbObj);
            memset(e, 0, cbObj);
            e->dwOfs = ofs;
            e->dwData = value;
            e->dwTimeStamp = GetTickCount();
            e->dwSequence = ++m_seq;
            n++;
        };
        Lock();
        if (m_kind == DEV_KEYBOARD) {
            for (int i = 0; i < 256; i++) {
                if (g_wantKeys[i] != m_sentKeys[i]) {
                    push((DWORD)i, g_wantKeys[i]);
                    m_sentKeys[i] = g_wantKeys[i];
                }
            }
        } else {
            InterlockedIncrement(&g_nMouseData);
            if (g_pendDx) { push(DIMOFS_X, (DWORD)g_pendDx); g_pendDx = 0; }
            if (g_pendDy) { push(DIMOFS_Y, (DWORD)g_pendDy); g_pendDy = 0; }
            for (int b = 0; b < 2; b++) {
                if (g_wantMouse[b] != m_sentMouse[b]) {
                    push(DIMOFS_BUTTON0 + b, g_wantMouse[b] ? 0x80 : 0);
                    m_sentMouse[b] = g_wantMouse[b];
                }
            }
        }
        Unlock();
        *inOut = n;
        return hr;
    }

    // pass-throughs
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS a) override { return m_real->GetCapabilities(a); }
    HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA a, LPVOID b, DWORD c) override { return m_real->EnumObjects(a, b, c); }
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID a, LPDIPROPHEADER b) override { return m_real->GetProperty(a, b); }
    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID a, LPCDIPROPHEADER b) override { return m_real->SetProperty(a, b); }
    HRESULT STDMETHODCALLTYPE Acquire() override { return m_real->Acquire(); }
    HRESULT STDMETHODCALLTYPE Unacquire() override { return m_real->Unacquire(); }
    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT a) override { return m_real->SetDataFormat(a); }
    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE a) override { return m_real->SetEventNotification(a); }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND a, DWORD b) override { return m_real->SetCooperativeLevel(a, b); }
    HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA a, DWORD b, DWORD c) override { return m_real->GetObjectInfo(a, b, c); }
    HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEA a) override { return m_real->GetDeviceInfo(a); }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND a, DWORD b) override { return m_real->RunControlPanel(a, b); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE a, DWORD b, REFGUID c) override { return m_real->Initialize(a, b, c); }
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID a, LPCDIEFFECT b, LPDIRECTINPUTEFFECT* c, LPUNKNOWN d) override { return m_real->CreateEffect(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKA a, LPVOID b, DWORD c) override { return m_real->EnumEffects(a, b, c); }
    HRESULT STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOA a, REFGUID b) override { return m_real->GetEffectInfo(a, b); }
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD a) override { return m_real->GetForceFeedbackState(a); }
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD a) override { return m_real->SendForceFeedbackCommand(a); }
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK a, LPVOID b, DWORD c) override { return m_real->EnumCreatedEffectObjects(a, b, c); }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE a) override { return m_real->Escape(a); }
    HRESULT STDMETHODCALLTYPE Poll() override { return m_real->Poll(); }
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD a, LPCDIDEVICEOBJECTDATA b, LPDWORD c, DWORD d) override { return m_real->SendDeviceData(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(LPCSTR a, LPDIENUMEFFECTSINFILECALLBACK b, LPVOID c, DWORD d) override { return m_real->EnumEffectsInFile(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(LPCSTR a, DWORD b, LPDIFILEEFFECT c, DWORD d) override { return m_real->WriteEffectToFile(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATA a, LPCSTR b, DWORD c) override { return m_real->BuildActionMap(a, b, c); }
    HRESULT STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATA a, LPCSTR b, DWORD c) override { return m_real->SetActionMap(a, b, c); }
    HRESULT STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA a) override { return m_real->GetImageInfo(a); }

private:
    IDirectInputDevice8A* m_real;
    DevKind m_kind;
    BYTE m_sentKeys[256] = {};
    bool m_sentMouse[2] = {false, false};
    DWORD m_seq = 0x40000000;
};

class WrapDI8 : public IDirectInput8A {
public:
    explicit WrapDI8(IDirectInput8A* real) : m_real(real) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        HRESULT hr = m_real->QueryInterface(riid, out);
        if (SUCCEEDED(hr) && *out == m_real) { *out = this; }
        return hr;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_real->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = m_real->Release();
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) override {
        HRESULT hr = m_real->CreateDevice(guid, out, outer);
        if (SUCCEEDED(hr) && out && *out) {
            if (IsEqualGUID(guid, GUID_SysKeyboard)) { *out = new WrapDevice(*out, DEV_KEYBOARD); Log("input: keyboard device wrapped"); }
            else if (IsEqualGUID(guid, GUID_SysMouse)) { *out = new WrapDevice(*out, DEV_MOUSE); Log("input: mouse device wrapped"); }
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD a, LPDIENUMDEVICESCALLBACKA b, LPVOID c, DWORD d) override { return m_real->EnumDevices(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID a) override { return m_real->GetDeviceStatus(a); }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND a, DWORD b) override { return m_real->RunControlPanel(a, b); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE a, DWORD b) override { return m_real->Initialize(a, b); }
    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID a, LPCSTR b, LPGUID c) override { return m_real->FindDevice(a, b, c); }
    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCSTR a, LPDIACTIONFORMATA b, LPDIENUMDEVICESBYSEMANTICSCBA c, LPVOID d, DWORD e) override { return m_real->EnumDevicesBySemantics(a, b, c, d, e); }
    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK a, LPDICONFIGUREDEVICESPARAMSA b, DWORD c, LPVOID d) override { return m_real->ConfigureDevices(a, b, c, d); }

private:
    IDirectInput8A* m_real;
};

void* Input_WrapDirectInput(void* realDI8A) {
    return realDI8A ? static_cast<IDirectInput8A*>(new WrapDI8((IDirectInput8A*)realDI8A)) : nullptr;
}
