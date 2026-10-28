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
#include "enginecam.h"
#include "combat.h"
#include "cues.h"
#include "anim.h"
#include "hover.h"
#include "melee.h"
#include "force.h"
#include "tutorial.h"
#include "dialogue.h"
#include "hud.h"
#include "actionring.h"

#pragma comment(lib, "dxguid.lib")

extern void Log(const char* fmt, ...);

// ---------------------------------------------------------------- desired state (computed per frame)
static BYTE g_wantKeys[256];
static double g_attackWatchAt = -10;  // an action we just started (swing / shot / use / talk): see the movement rules in Input_Update
static float g_attackFrom[3];
static void WatchAttack(double now) {
    if (Weapon_PlayerPos(g_attackFrom)) g_attackWatchAt = now;
}   // DIK_* -> 0x80 when held
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
static double g_pickStartMs = -1;  // world grip pick in progress (GetTickCount ms), see below
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
        // the view's heading is ours (enginecam), the game camera's may differ: rotate by both
        float psi = VR_HeadYaw() + EngineCam_ViewYawOffset();
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
        // turning: right stick X turns our view directly (smooth, or snap steps with [Comfort] SnapTurn=1); the game's
        // own camera keys are not used, so KotOR can neither ignore the turn nor swing the view around in combat
        {
            static double lastT = -1;
            static bool snapArmed = true;
            double tnow = GetTickCount() / 1000.0;
            float dt = lastT > 0 ? (float)min(tnow - lastT, 0.1) : 0;
            lastT = tnow;
            const float deg = 3.14159265f / 180.0f;
            if (Features().snapTurn) {
                if (fabsf(in.rx) > 0.7f && snapArmed) {
                    EngineCam_Turn((in.rx > 0 ? -1.0f : 1.0f) * Features().snapAngle * deg);
                    snapArmed = false;
                } else if (fabsf(in.rx) < 0.3f) {
                    snapArmed = true;
                }
            } else if (fabsf(in.rx) > 0.15f) {
                float k = (fabsf(in.rx) - 0.15f) / 0.85f;
                EngineCam_Turn((in.rx > 0 ? -1.0f : 1.0f) * k * k * Features().turnSpeed * deg * dt);
            }
        }
        // ---- motion gestures (melee swings, shots, Force moves) and the attacks they trigger
        float headPos[3] = {0, 0, 0};
        VR_HeadPos(headPos);
        Gestures_Update(in, headPos);
        Combat_Update();
        Cues_Update();
        Tutorial_Update();
        Hud_Update();
        Anim_Update();
        Hover_Update(in);
        Melee_Update();
        double now = Gestures_Now();
        bool ringWasOpen = ActionRing_Active();
        if (BYTE k = ActionRing_Update(in)) {  // trigger on an action in the ring: its column's key
            g_gestureKey = k;
            g_gestureKeyAt = now;
        }
        if (BYTE k = Dialogue_Update(in)) {  // trigger on a reply in the conversation panel: its number key
            g_gestureKey = k;
            g_gestureKeyAt = now;
        }
        float swingQ;
        // Attacks go through KotOR's Default Action (R) on the current target: clicking would make the character
        // walk to the floor when the swing / shot isn't over an enemy. Grip selects / uses what the hand points at.
        // the world context: the right hand points into the world, or the left hand is on the HUD (the right hand
        // and its trigger still belong to the weapon then)
        bool worldCtx = in.pointerMode == KVR_POINTER_WORLD || in.pointerMode == KVR_POINTER_HUD_LEFT;
        bool inWorld = worldCtx && Features().gestures;
        bool fighting = inWorld && Weapon_PlayerInCombat();  // R outside combat would *use* whatever is targeted
        // Movement stays yours. (1) Pushing the stick while the game is still running an attack for you clears it
        // (the HUD's clear-queue button): an attack on a target that can't be reached used to leave the character
        // unable to walk after a fight. (2) An action we start (swing, shot, use, open, talk) must never walk you to
        // the target: if the character travels > 0.8 m with the stick idle in the next 2.5 s, that walk is cancelled
        // and a double tick says "too far - step closer".
        {
            static bool stickWas;
            bool stickNow = mag > 0.5f;
            if (stickNow && !stickWas && Combat_PlayerAttacking() && ActionRing_ClearQueue()) {
                Combat_ResetAttacking();
                g_attackWatchAt = -10;
                Log("input: stick -> queued attack cleared (movement is yours)");
            }
            stickWas = stickNow;
            if (now - g_attackWatchAt < 2.5) {
                float pp[3];
                if (Weapon_PlayerPos(pp)) {
                    float dx = pp[0] - g_attackFrom[0], dy = pp[1] - g_attackFrom[1];
                    if (mag < 0.3f && dx * dx + dy * dy > 0.8f * 0.8f && ActionRing_ClearQueue()) {
                        Combat_ResetAttacking();
                        g_attackWatchAt = -10;
                        Log("input: action would walk you to the target -> cancelled (step closer)");
                        VR_Haptic(1, 0.5f, 0.04f, 90);
                        Cues_TooFar();
                    }
                }
            }
        }
        // v2 contact: touching an enemy with the blade starts your attack on it (no walking); swings themselves
        // no longer press the attack key, so a guard raised into a blow is never read as an attack
        if (Combat_ContactMode()) {
            DWORD startOn = Melee_TakeAttackStart();
            if (startOn && fighting && !Combat_PlayerAttacking()) {
                Weapon_SetTarget(startOn | 0x80000000);
                g_gestureKey = DIK_R;
                g_gestureKeyAt = now;
                WatchAttack(now);
                Log("input: blade contact -> attack %08lX", startOn);
            }
        }
        if (Gestures_TakeSwing(&swingQ) && fighting && !Combat_ContactMode()) {
            if (Combat_PlayerAttacking()) {  // KotOR repeats the attack by itself: the swing only counts for timing
                Log("input: swing (already attacking: timing only)");
            } else {
                g_gestureKey = DIK_R;
                g_gestureKeyAt = now;
                WatchAttack(now);
                Log("input: swing -> attack current target");
            }
            if (Combat_InPerfectWindow()) Cues_Fire(CUE_PERFECT_HIT);
        }
        static bool trigWas;
        // v2: the bolt flies on the next drawn frame; whatever live enemy it hit becomes the target (then R)
        static double pendingShotTarget = -10;
        if (pendingShotTarget > 0) {
            DWORD hit = Melee_TakeShotTarget();
            if (hit) {
                Weapon_SetTarget(hit | 0x80000000);
                g_gestureKey = DIK_R;
                g_gestureKeyAt = now;
                WatchAttack(now);
                Log("input: shot hit %08lX -> target + attack", hit);
                pendingShotTarget = -10;
            } else if (now - pendingShotTarget > 0.3) {
                pendingShotTarget = -10;
            }
        }
        bool trigNow = in.trigR > 0.6f;
        static bool rangedPickWanted;
        static double lastRangedPick = -10;
        bool rangedHeld = !Weapon_Profile(Weapon_Right()).melee && Weapon_Right() != WC_NONE;
        static double trigDownAt = -10, lastShotAt = -10;
        // a gun fires only in a fight, and never while a conversation is up (answering used to shoot)
        if (trigNow && !trigWas && fighting && rangedHeld && !ringWasOpen && !Dialogue_Showing() && !Force_MenuHovered()) {
            // a gun fires when you pull its trigger: bolt, flash, recoil and sound right away; where the barrel points
            // decides what it hits (v2). Fire modes: a steady second before the pull = Sniper Shot, a quick second pull
            // = Rapid Shot, a trigger held ~1 s and let go = Power Blast (an extra, heavier bolt).
            if (Weapon_Steady()) Gestures_QueueMove(MOVE_SNIPER);
            else if (now - lastShotAt < 0.45) Gestures_QueueMove(MOVE_RAPID);
            Weapons_FireAlongBarrel();
            Cues_Shot();
            trigDownAt = now;
            lastShotAt = now;
        }
        if (!trigNow && trigWas && inWorld && rangedHeld && trigDownAt > 0 && now - trigDownAt >= 0.8) {
            Gestures_QueueMove(MOVE_POWER_SHOT);
            Weapons_ChargeNextShot();
            Weapons_FireAlongBarrel();
            Cues_Shot();
            VR_Haptic(1, 1.0f, 0.12f, 70);
            Log("input: Power Blast");
            trigDownAt = -10;
        }
        if (trigNow && !trigWas && fighting && !ringWasOpen) {
            bool ranged = rangedHeld;
            if (ranged) {
                Gestures_OnShot();
                // shoot what you point at: the game already hovers what the hand points at (the hover ring) - make
                // it the target and attack. No aim pick: its hidden camera frames were the lag spike on every shot
                // (2026-10-27). At most once a second (KotOR keeps firing every round on its own).
                DWORD hover = 0;
                if (now - lastRangedPick > 1.0 && Combat_ContactMode()) pendingShotTarget = now;  // see below
                if (now - lastRangedPick > 1.0 && !Combat_ContactMode() && Weapon_HoverClientId(&hover)) {
                    Weapon_SetTarget(hover);
                    Hover_Interacted();
                    g_gestureKey = DIK_R;
                    g_gestureKeyAt = now;
                    WatchAttack(now);
                    lastRangedPick = now;
                    Log("input: shot -> target %08lX + attack", hover);
                }
            } else if (!Combat_PlayerAttacking()) {
                g_gestureKey = DIK_R;
                g_gestureKeyAt = now;
                WatchAttack(now);
            }
            if (Combat_InPerfectWindow()) Cues_Fire(CUE_PERFECT_HIT);
        }
        trigWas = trigNow;
        if (Combat_ContactMode()) {  // v2: gather with the left grip, release to cast (force.cpp)
            if (BYTE k = Force_Update(in, fighting)) {
                g_gestureKey = k;
                g_gestureKeyAt = now;
            }
        } else if (Gestures_TakeForcePush() && fighting) {  // target slot 2: Force powers - on the enemy your palm points at
            DWORD aimed = Melee_PalmTarget();
            if (aimed) {
                Weapon_SetTarget(aimed | 0x80000000);
                Log("input: Force push aimed at %08lX", aimed);
            }
            g_gestureKey = DIK_2;
            g_gestureKeyAt = now;
            Cues_Fire(CUE_FORCE);
        }
        if (!Combat_ContactMode() && Gestures_TakeForcePull() && inWorld) { g_gestureKey = DIK_4; g_gestureKeyAt = now; Cues_Fire(CUE_FORCE); }  // personal power slot
        if (g_gestureKeyAt >= 0) {
            if (now - g_gestureKeyAt < 0.1) keys[g_gestureKey] = 0x80;
            else g_gestureKeyAt = -1;
        }

        // pause - never during a conversation or just after one (A used to end conversations in pause mode)
        if ((in.buttons & KVR_BTN_A) && !Dialogue_Showing() && Dialogue_EndedAgo() > 1.5) keys[DIK_SPACE] = 0x80;
        static bool eatB;  // B that closed the action ring is not also Escape
        if (ringWasOpen && (in.buttons & KVR_BTN_B)) eatB = true;
        if (!(in.buttons & KVR_BTN_B)) eatB = false;
        if (((in.buttons & KVR_BTN_MENU) || ((in.buttons & KVR_BTN_B) && !Dialogue_Showing())) && !eatB)
            keys[DIK_ESCAPE] = 0x80;  // menu / back (B in a conversation: skip the line)
        if (in.buttons & KVR_BTN_X) keys[DIK_TAB] = 0x80;                       // change character
        // click: in menus the right trigger on the panel; in the world the HUD only answers the LEFT hand's trigger, so
        // the right trigger always stays the weapon. Each press keeps the owner it started with until released (a shot
        // that drifts onto a panel never becomes a click, and the other way round).
        bool world = worldCtx;
        static int rOwner;  // right trigger press: 0 none, 1 world / weapon, 2 panel click
        bool rDown = in.trigR > 0.6f;
        if (!rDown) rOwner = 0;
        else if (!rOwner) rOwner = in.pointerMode == KVR_POINTER_PANEL ? 2 : 1;
        static int lOwner;  // left trigger press: 0 none, 1 HUD click, 2 other (right mouse button in menus)
        bool lDown = in.trigL > 0.6f;
        if (!lDown) lOwner = 0;
        else if (!lOwner) lOwner = in.pointerMode == KVR_POINTER_HUD_LEFT ? 1 : 2;
        mouse[0] = ((rOwner == 2 && in.pointerMode == KVR_POINTER_PANEL) ||
                    (lOwner == 1 && in.pointerMode == KVR_POINTER_HUD_LEFT) ||
                    (in.pointerMode == KVR_POINTER_PANEL && in.poke)) &&  // a fingertip poke on the wrist panel
                   !Dialogue_Active() ||
                   Dialogue_SkipClick();  // A / B during a spoken line: the click that skips it
        // World grip: no mouse click at all (KotOR draws the target's action menu right at the target, so a click there
        // fires a menu slot: the medpack / mine accidents). Instead the game camera looks down the hand's aim ray for
        // ~0.35 s (frames not shown in the headset), the cursor hovers the screen center so the aimed object becomes
        // the target, and KotOR's Default Action key (R) uses / talks to / opens / attacks it.
        // Held on (>= 0.4 s) instead: the target's other actions open in the action ring (security, bash, Force powers,
        // feats, ...) and no default action is taken (none when the target has no others: then R as usual).
        static bool gripWas;
        static bool gripPick;        // the running pick came from the grip (not a shot)
        static double gripDownMs;
        static bool deferR;          // grip still held when the pick settled: R on a short release, ring on a hold
        bool gripNow = world && in.gripR > 0.7f && !ringWasOpen;
        double tick = (double)GetTickCount();
        // grip on what the hover ring marks: target it directly (no aim pick, no hidden frames); the aim pick stays
        // the fallback when the hand points at nothing the game hovers
        DWORD gripHover;
        if (gripNow && !gripWas && !rangedPickWanted && g_pickStartMs < 0 && Weapon_HoverClientId(&gripHover)) {
            Weapon_SetTarget(gripHover);
            Hover_Interacted();
            gripPick = true;
            gripDownMs = tick;
            deferR = true;  // short squeeze -> R, held -> the action ring
            Log("input: world grip -> target %08lX (hover)", gripHover);
        } else if (((gripNow && !gripWas) || rangedPickWanted) && g_pickStartMs < 0) {
            gripPick = !rangedPickWanted;
            gripDownMs = tick;
            deferR = false;
            rangedPickWanted = false;
            int w, h;
            VR_ClientSize(&w, &h);
            if (w > 0) {
                VR_BeginAimPick(in.aim, 0.35);
                g_autoTx = w * 0.5f;
                g_autoTy = h * 0.5f;
                g_pickStartMs = tick;
                Log("input: world grip -> aim pick + default action");
            }
        }
        if (deferR) {
            if (!gripNow) {  // short squeeze: the default action, as always
                deferR = false;
                g_gestureKey = DIK_R;
                g_gestureKeyAt = now;
                if (fighting) WatchAttack(now);  // an attack never walks you; a use / talk / open does, as in the game
            } else if (tick - gripDownMs >= 400) {
                deferR = false;
                if (!ActionRing_Open(in)) {
                    g_gestureKey = DIK_R;
                    g_gestureKeyAt = now;
                    if (fighting) WatchAttack(now);
                } else {
                    Log("input: grip held -> action ring");
                }
            }
        }
        gripWas = gripNow;
        mouse[1] = lOwner == 2 && !world;  // the left trigger is the right mouse button in menus only

        // cursor steering toward the pointer target
        float tx, ty;
        bool haveTarget = false;
        if (in.pointerMode == KVR_POINTER_PANEL || in.pointerMode == KVR_POINTER_HUD_LEFT) {
            int w, h;
            VR_ClientSize(&w, &h);
            tx = in.panelU * w;
            ty = in.panelV * h;
            haveTarget = w > 0;
        } else if (in.pointerMode == KVR_POINTER_WORLD) {
            haveTarget = VR_WorldPointerToClient(in.aim.pos, in.aim.quat, &tx, &ty);
        }
        if (g_pickStartMs >= 0) {
            double t = tick - g_pickStartMs;
            if (t >= 120 && g_gestureKeyAt < 0 && t < 200 && !deferR) {  // hover has settled on the aimed object
                if (gripPick && gripNow) {
                    deferR = true;
                } else {
                    g_gestureKey = DIK_R;
                    g_gestureKeyAt = now;
                    if (!gripPick || fighting) WatchAttack(now);  // a shot / attack never walks you
                }
            }
            if (t > 330) g_pickStartMs = -1;
            tx = g_autoTx;
            ty = g_autoTy;
            haveTarget = true;
            mouse[0] = false;
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
            // During an aim pick the cursor sits still at the screen centre while the camera turns down the hand's
            // ray: nudge it a pixel every frame so the game re-resolves what is under it with the aimed camera.
            // (A single move on the pick's first frame was resolved against the old view - often the NPC in front
            // of you: grips on doors started conversations with Trask.)
            if (g_pickStartMs >= 0) {
                scr.x += (GetTickCount() / 16) & 1;
                client = scr;
                ScreenToClient(dst, &client);
            }
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
