// Conversations in the world.
//
// KotOR's conversation panels (the NPC dialog and the computer terminal share this code) are driven through:
//   Subtitle    0x6A7AE0  thiscall (CExoString*), ret 4: the NPC dialog's line (the cinematic subtitle bar); used
//                         by NPC conversations in place of SetMessage
//   Reset       0x6A7160  thiscall (): first, for every node
//   SetMessage  0x6A7010  thiscall (CExoString*), ret 4: the speaker's line (vtable slot 0x1D4; the computer overrides
//                         it and calls this base). The reply list stays empty until SetReplies follows (at once, or
//                         once a scripted pause / voice line is over)
//   SetReplies  0x6A86A0  thiscall (count, CExoString texts[count] (char* + length, 8 bytes each), ids, ?, show),
//                         ret 0x14 (computer: 0x6A94E0 calls this)
//   SelectReply 0x6A7110  thiscall (index), ret 4: a reply was picked (the input handler maps the keys 1..9 to it)
//   panel+0x1C            non-null exactly while the panel is up (0 before it opens and once the conversation ends)
//
// We mirror the panel onto a host text panel (slot 1) world-locked in front of you: the speaker's line as the title,
// the replies below it. The host hit-tests the right-hand ray against the lines (KvrTextPanel::pickable / hover); a
// trigger pull taps the hovered reply's number key, so KotOR's own selection path runs. The game's flat panel keeps
// working too.
//
// Lines without replies (NPC lines, cutscene subtitles) come from the cinematic panel, not the reply panel: the panel
// shows while any dialog panel is up (GUI manager), so every line is captioned. A / B skips the voiced line (Enter,
// the dialog panel input handler 0x6A7230 skip while the line plays). Computer terminals (CSWGuiDialogComputer) are not mirrored: their own screen is
// shown on the flat panel with the laser (gui.cpp).
#include "dialogue.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../common/kvr_shared.h"
#include "gui.h"
#include "enginecam.h"
#include "movie.h"
#include "weapons.h"
#include "vr.h"

extern void Log(const char* fmt, ...);

namespace {

const DWORD kSetMessageVA = 0x6A7010;
const BYTE kSetMessageOrig[9] = {0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0x08, 0x1A, 0x00, 0x00};  // push esi; mov esi, ecx; mov ecx, [esi+0x1A08]
const DWORD kSubtitleVA = 0x6A7AE0;
const BYTE kSubtitleOrig[8] = {0xA1, 0xFC, 0x39, 0x7A, 0x00, 0x83, 0xEC, 0x18};  // mov eax, [app]; sub esp, 0x18
const DWORD kResetVA = 0x6A7160;
const DWORD kSetRepliesVA = 0x6A86A0;
const BYTE kFramePrologue[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};  // push ebp; mov ebp, esp; and esp, -8 (both)
const DWORD kSelectVA = 0x6A7110;
const BYTE kSelectOrig[7] = {0x8B, 0x41, 0x6C, 0x8B, 0x54, 0x24, 0x04};  // mov eax, [ecx+0x6C]; mov edx, [esp+4]
const int kOpenOffset = 0x1C;
const int kSlot = 1;

BYTE* g_messageTramp;
BYTE* g_subtitleTramp;
BYTE* g_resetTramp;
BYTE* g_repliesTramp;
BYTE* g_selectTramp;
CRITICAL_SECTION g_lock;
std::string g_message;               // the speaker's line (UTF-8)
std::vector<std::string> g_replies;  // as shown, "1. ..." (UTF-8)
volatile bool g_dirty;
void* volatile g_panel;       // the panel that last sent text
void* volatile g_replyPanel;  // the panel holding the replies (NPC conversations send the line from another object)
bool g_showing;
int g_shownReplies;
bool g_trigWas;
DWORD g_skipAt;  // B skip pressed (GetTickCount), 0 = never
DWORD g_endedAt;  // the last conversation ended (GetTickCount), 0 = never

std::string ToUtf8(const char* s, int len) {
    if (!s || len <= 0) return std::string();
    std::vector<wchar_t> w(len + 1);
    int n = MultiByteToWideChar(1252, 0, s, len, w.data(), len);
    std::string out(n * 3 + 1, '\0');
    int m = WideCharToMultiByte(CP_UTF8, 0, w.data(), n, &out[0], (int)out.size(), nullptr, nullptr);
    out.resize(m > 0 ? m : 0);
    return out;
}

int CopyExo(const void* exo, char* buf, int cap) {  // CExoString: char* text, int length
    __try {
        const char* p = *(const char* const*)exo;
        int len = *(const int*)((const BYTE*)exo + 4);
        if (!p || len <= 0) return 0;
        int n = (int)strnlen(p, len < cap ? len : cap);
        memcpy(buf, p, n);
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

std::string ReadExo(const void* exo) {
    char buf[1024];
    return ToUtf8(buf, CopyExo(exo, buf, sizeof buf));
}

bool IsComputer(void* panel) {  // CSWGuiDialogComputer (constructor 0x6A8EB0) and its info / base vtables
    __try {
        DWORD vt = *(DWORD*)panel;
        return vt == 0x755888 || vt == 0x755730;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

}  // namespace

extern "C" void __cdecl Dialogue_MessagePre(void* panel, const void* exo) {
    if (IsComputer(panel)) return;
    std::string m = exo ? ReadExo(exo) : std::string();
    if (m.find_first_not_of(" \t\r\n") == std::string::npos) m.clear();  // blank lines (the opening's) are no caption
    Log("dialogue: message (panel %p) t=%lu: %.40s", panel, GetTickCount(), m.c_str());
    // An empty message is a panel clearing its subtitle - often the other dialog panel, 16 ms after the real line
    // arrived (2026-10-27 log): it used to wipe the caption and its replies. Keep the last line until the next one.
    if (m.empty()) return;
    EnterCriticalSection(&g_lock);
    g_panel = panel;
    g_message.swap(m);
    g_replies.clear();
    g_dirty = true;
    LeaveCriticalSection(&g_lock);
}

extern "C" void __cdecl Dialogue_ResetPre(void* panel) {
    if (panel == g_panel) Log("dialogue: reset t=%lu", GetTickCount());
}

extern "C" void __cdecl Dialogue_RepliesPre(void* panel, int count, const BYTE* texts) {
    if (IsComputer(panel)) return;
    std::vector<std::string> r;
    if (count > 20) count = 20;
    for (int i = 0; i < count && texts; i++) {
        std::string t = ReadExo(texts + i * 8);
        if (t.empty()) t = "[Continue]";  // KotOR's "continue" entries have no text
        size_t hint = t.find(" [Left-click");  // the opening's mouse tutorial hint means nothing in VR
        if (hint != std::string::npos) t.erase(hint);
        char num[8];
        sprintf_s(num, "%d. ", i + 1);
        r.push_back((i < 9 ? num : "") + t);
    }
    EnterCriticalSection(&g_lock);
    g_replies.swap(r);
    g_dirty = true;
    g_panel = panel;
    g_replyPanel = panel;
    LeaveCriticalSection(&g_lock);
    Log("dialogue: %d replies (panel %p) t=%lu", count, panel, GetTickCount());
}

extern "C" void __cdecl Dialogue_SelectPre(void* panel, int index) {
    if (panel == g_replyPanel) Log("dialogue: reply %d chosen t=%lu", index + 1, GetTickCount());
}

__declspec(naked) static void MessageStub() {
    __asm {
        push ecx
        push dword ptr [esp + 8]     // CExoString* (arg 1)
        push ecx                     // the panel
        call Dialogue_MessagePre
        add esp, 8
        pop ecx
        jmp dword ptr [g_messageTramp]
    }
}

__declspec(naked) static void SubtitleStub() {
    __asm {
        push ecx
        push dword ptr [esp + 8]     // CExoString* (arg 1)
        push ecx                     // the panel
        call Dialogue_MessagePre
        add esp, 8
        pop ecx
        jmp dword ptr [g_subtitleTramp]
    }
}

__declspec(naked) static void ResetStub() {
    __asm {
        push ecx
        push ecx
        call Dialogue_ResetPre
        add esp, 4
        pop ecx
        jmp dword ptr [g_resetTramp]
    }
}

__declspec(naked) static void RepliesStub() {
    __asm {
        push ecx
        push dword ptr [esp + 0xC]   // texts (arg 2)
        push dword ptr [esp + 0xC]   // count (arg 1)
        push ecx                     // the panel
        call Dialogue_RepliesPre
        add esp, 12
        pop ecx
        jmp dword ptr [g_repliesTramp]
    }
}

__declspec(naked) static void SelectStub() {
    __asm {
        push ecx
        push dword ptr [esp + 8]     // index (arg 1)
        push ecx                     // the panel
        call Dialogue_SelectPre
        add esp, 8
        pop ecx
        jmp dword ptr [g_selectTramp]
    }
}

static bool Detour(DWORD va, const BYTE* orig, int len, void* stub, BYTE** tramp, const char* what) {
    BYTE* target = (BYTE*)(ULONG_PTR)va;
    if (memcmp(target, orig, len) != 0) {
        Log("dialogue: unexpected bytes at %08lX (%s), not hooking", va, what);
        return false;
    }
    *tramp = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(*tramp, target, len);
    (*tramp)[len] = 0xE9;
    *(INT32*)(*tramp + len + 1) = (INT32)((target + len) - (*tramp + len + 5));
    DWORD old;
    VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xE9;
    *(INT32*)(target + 1) = (INT32)((BYTE*)stub - (target + 5));
    for (int i = 5; i < len; i++) target[i] = 0x90;
    VirtualProtect(target, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, len);
    Log("dialogue: hooked %s at %08lX", what, va);
    return true;
}

bool Dialogue_Install() {
    InitializeCriticalSection(&g_lock);
    bool ok = Detour(kSetMessageVA, kSetMessageOrig, 9, MessageStub, &g_messageTramp, "SetMessage");
    ok &= Detour(kSubtitleVA, kSubtitleOrig, 8, SubtitleStub, &g_subtitleTramp, "Subtitle");
    ok &= Detour(kResetVA, kFramePrologue, 6, ResetStub, &g_resetTramp, "Reset");
    ok &= Detour(kSetRepliesVA, kFramePrologue, 6, RepliesStub, &g_repliesTramp, "SetReplies");
    ok &= Detour(kSelectVA, kSelectOrig, 7, SelectStub, &g_selectTramp, "SelectReply");
    return ok;
}

bool Dialogue_Active() {
    const KvrShared* shm = VR_Shared();
    return g_showing && shm && shm->text[kSlot].hover >= 0;
}

// The dialog panel's line state (+0x1DF8): bit 1 = the line is playing, bit 2 = it can't be skipped. -1 unreadable.
static int LineFlags(void* p) {
    if (!p) return -1;
    __try {
        return *(int*)((BYTE*)p + 0x1DF8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}

// the conversation's owner (CSWGuiDialog::SetOwner 0x6A7C90 writes +0x19C0); 0 unknown / yourself
static DWORD SpeakerOf(void* panel) {
    if (!panel) return 0;
    __try {
        DWORD id = *(DWORD*)((BYTE*)panel + 0x19C0);
        BYTE* p = (BYTE*)Weapon_PlayerCreature();
        if (id == 0 || id == 0x7F000000 || (p && (id & 0x7FFFFFFF) == *(DWORD*)(p + 4))) return 0;
        return id;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

static bool PanelOpen(void* p) {
    if (!p) return false;
    __try {
        return *(void* const*)((const BYTE*)p + kOpenOffset) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// Pick a reply the way the panel's input handler 0x6A7230 does for a reply key (event 0xFE + n): its virtual method
// at vtable + 0x6C with the index. Called directly, because the handler only receives keys once the panel has had
// keyboard focus - in a new game's first conversation it ignored our number keys until a mouse click.
static bool SelectReplyDirect(void* panel, int index) {
    typedef void(__fastcall * SelectFn)(void* self, void* edx, int index);  // thiscall, one stack argument
    __try {
        BYTE* vt = *(BYTE**)panel;
        if ((ULONG_PTR)vt < 0x700000 || (ULONG_PTR)vt > 0x7A0000) return false;  // the exe's vtables
        SelectFn fn = *(SelectFn*)(vt + 0x6C);
        if ((ULONG_PTR)fn < 0x401000 || (ULONG_PTR)fn > 0x734000) return false;  // the exe's code
        fn(panel, nullptr, index);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

BYTE Dialogue_Update(const KvrInput& in) {
    const KvrShared* shm = VR_Shared();
    if (!shm) return 0;
    // shown only while the game's panel is up: it opens a moment after the first line arrives, and the closing node's
    // last updates land as it closes
    // while a conversation / cutscene panel is shown (GUI manager list; the reply panel's own open flag is only set
    // while replies are up, so lines between reply rounds went uncaptioned)
    // only while the game shows a conversation / cutscene panel (GUI manager list). The reply panel's own "open" pointer
    // (+0x1C) is not reliable: after the tutorial conversation it stayed set, our panel never closed and its stale
    // reply re-started the conversation when picked (2026-10-27 softlock).
    bool open = Gui_State().dialogPanels > 0;
    // a cinematic movie plays: step aside (it came back with the next line) - 2026-10-27
    if (Movie_Playing()) {
        if (g_showing) {
            VR_HideText(kSlot);
            g_showing = false;
            g_dirty = true;  // re-shown with the current line when the movie ends
        }
        return 0;
    }
    if (g_showing && !open) {
        Log("dialogue: over");
        VR_HideText(kSlot);
        g_showing = false;
        g_endedAt = GetTickCount();
        EnterCriticalSection(&g_lock);  // nothing of it may be picked or shown again
        g_message.clear();
        g_replies.clear();
        g_shownReplies = 0;
        g_dirty = false;
        LeaveCriticalSection(&g_lock);
    }
    if (g_dirty && open) {
        EnterCriticalSection(&g_lock);
        g_dirty = false;
        if (g_message.empty() && g_replies.empty()) {  // nothing to show (an empty prompt): no panel
            LeaveCriticalSection(&g_lock);
            if (g_showing) VR_HideText(kSlot);
            g_showing = false;
            return 0;
        }
        // line 0 is always the title (the host never offers it for picking)
        std::string text = "#" + (g_message.empty() ? std::string("...") : g_message);
        for (auto& r : g_replies) text += "\n" + r;
        g_shownReplies = (int)g_replies.size();
        LeaveCriticalSection(&g_lock);
        if (!g_showing) {
            // world-locked in the direction of whoever you are talking to (the dialog panel's owner, +0x19C0), a
            // little low so they stay visible above it; where you look when there is no one to face
            float head[3];
            VR_HeadPos(head);
            float yaw = VR_HeadYaw();
            float spk[3], st[3];
            DWORD owner = SpeakerOf(g_replyPanel ? g_replyPanel : g_panel);
            if (owner && Weapon_ObjectPos(owner & 0x7FFFFFFF, spk)) {
                spk[2] += 1.2f;
                if (EngineCam_WorldToTracking(spk, st)) {
                    float dx = st[0] - head[0], dz = st[2] - head[2];
                    if (dx * dx + dz * dz > 0.3f * 0.3f) {
                        yaw = atan2f(-dx, -dz);  // facing direction: -Z forward
                        Log("dialogue: panel placed toward the speaker %08lX (%.0f deg)", owner, yaw * 57.29578f);
                    }
                }
            }
            float pos[3] = {head[0] - sinf(yaw) * 1.1f, head[1] - 0.45f, head[2] - cosf(yaw) * 1.1f};
            // yaw to face you, tilted back ~20 deg so it looks up at your eyes
            float sy = sinf(yaw * 0.5f), cy = cosf(yaw * 0.5f), sp = sinf(-0.175f), cp = cosf(-0.175f);
            float q[4] = {cy * sp, sy * cp, -sy * sp, cy * cp};
            VR_ShowText(kSlot, KVR_ANCHOR_WORLD, pos, q, 1.0f, text.c_str(), -1);
        } else {  // same place, new lines
            const KvrTextPanel& t = shm->text[kSlot];
            VR_ShowText(kSlot, KVR_ANCHOR_WORLD, t.pose.pos, t.pose.quat, t.width, text.c_str(), -1);
        }
        VR_SetTextPickable(kSlot, true);
        g_showing = true;
    }
    // pick: trigger on a hovered reply -> the panel's own selection (the number key only as a fallback)
    BYTE key = 0;
    bool trig = in.trigR > 0.6f;
    if (g_showing && trig && !g_trigWas) {
        int reply = shm->text[kSlot].hover - 1;
        static const BYTE kDigits[9] = {0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};  // DIK_1..DIK_9
        if (reply >= 0 && reply < 9 && reply < g_shownReplies) {
            bool direct = g_replyPanel && SelectReplyDirect(g_replyPanel, reply);
            if (!direct) key = kDigits[reply];
            Log("dialogue: picked reply %d (%s)", reply + 1, direct ? "selected on the panel" : "number key");
        }
    }
    g_trigWas = trig;
    // skip the line being spoken: A or B -> Enter, which the panel's input handler (0x6A7230) turns into its skip
    // while the line plays (panel flags +0x1DF8: bit 1 set, bit 2 clear). Once the line is over Enter would pick the
    // highlighted reply instead, so it is only sent while the line plays.
    static bool skipWas;
    bool skip = (in.buttons & KVR_BTN_B) != 0;  // B only: A is pause
    if (g_showing && skip && !skipWas && !key) {
        int flags = LineFlags(g_replyPanel ? g_replyPanel : g_panel);
        if (flags >= 0 && (flags & 1) && !(flags & 2)) {
            key = 0x1C;  // DIK_RETURN
            Log("dialogue: line skipped");
        } else {
            Log("dialogue: skip ignored (the line is over, flags %d)", flags);
        }
    }
    skipWas = skip;
    return key;
}

bool Dialogue_SkipClick() { return false; }

double Dialogue_EndedAgo() { return g_endedAt ? (GetTickCount() - g_endedAt) / 1000.0 : 1e9; }

bool Dialogue_Showing() { return g_showing;
}
